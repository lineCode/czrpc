#pragma once

namespace cz
{
namespace rpc
{

// Small utility struct to make it easier to work with the RPC headers
struct Header
{
	enum
	{
		kSizeBits = 32,
		kRPCIdBits = 7,
		kCounterBits = 22,
	};
	explicit Header()
	{
		static_assert(sizeof(*this) == sizeof(uint64_t), "Invalid size. Check the bitfields");
		all_ = 0;
	}

	struct Bits
	{
		unsigned size : kSizeBits;
		unsigned counter : kCounterBits;
		unsigned rpcid : kRPCIdBits;
		unsigned hasDbg : 1;   // Does this have a debug section before the payload ?
		unsigned isReply : 1;  // Is it a reply to a RPC call ?
		unsigned success : 1;  // Was the RPC call a success ?
	};

	uint32_t key() const { return (bits.counter << kRPCIdBits) | bits.rpcid; }
	bool isGenericRPC() const { return bits.rpcid == 0; }

	union {
		Bits bits;
		uint64_t all_;
	};
};

inline Stream& operator<<(Stream& s, const Header& v)
{
	s << v.all_;
	return s;
}

inline Stream& operator>>(Stream& s, Header& v)
{
	s >> v.all_;
	return s;
}

struct DebugInfo
{
	unsigned num;
	int line;
	char file[256];
	DebugInfo() {}
	DebugInfo(const char* file_, int line_)
	{
		static_assert(
			sizeof(*this) == (sizeof(num)+sizeof(line)+sizeof(file)),
			"Cannot have padding");
		static std::atomic<unsigned> counter(0);
		num = counter.fetch_add(1);
		line = line_;
		copyStrToFixedBuffer(file, file_);
	}
};

inline Stream& operator<<(Stream& s, const DebugInfo& v)
{
	s.write(&v, sizeof(v));
	return s;
}

inline Stream& operator>>(Stream& s, DebugInfo& v)
{
	s.read(&v, sizeof(v));
	return s;
}

struct InProcessorData
{
	InProcessorData(void* owner)
		: objData(owner)
	{
	}

	~InProcessorData()
	{
		auto tmp = pending([&](PendingFutures& pending)
		{
			decltype(pending.futures) tmp;
			pending.futures.swap(tmp);
			return tmp;
		});

		// this will cause all futures to block in the destructor, so all continuations can finish
		tmp.clear();

		pending([&](PendingFutures& pending)
		{
			pending.done.clear();
			CZRPC_ASSERT(pending.futures.size() == 0);
		});
	}

	struct PendingFutures
	{
		unsigned counter = 0;
		std::unordered_map<unsigned, std::future<void>> futures;
		std::vector<std::future<void>> done;
	};
	Monitor<PendingFutures> pending;
	ObjectData objData;
	bool authPassed = false;

	//
	// Control RPCS
	//
	Any getProperty(std::string name)
	{
		return objData.getProperty(name);
	}

	Any setProperty(std::string name, Any val)
	{
		return Any(objData.setProperty(name, std::move(val), true));
	}
	Any auth(const std::string token)
	{
		authPassed = objData.checkAuthToken(token);
		return Any(authPassed);
	}
};

//
// Helper code to dispatch a call.
namespace details
{

struct Send
{
	static void error(Transport& trp, Header hdr, const char* what, DebugInfo* dbg)
	{
		Stream o;
		o << hdr; // reserve space for the header
		o << what;
		hdr.bits.hasDbg = false;
		hdr.bits.isReply = true;
		hdr.bits.success = false;
		hdr.bits.size = o.writeSize();
		*reinterpret_cast<Header*>(o.ptr(0)) = hdr;
		if (dbg)
		{
			CZRPC_LOG(Log, CZRPC_LOGSTR_REPLY"size=%u, exception=%s",
				dbg->num, o.writeSize(),what);
		}
		trp.send(o.extract());
	}

	static void result(Transport& trp, Header hdr, Stream& o, DebugInfo* dbg)
	{
		hdr.bits.hasDbg = false;
		hdr.bits.isReply = true;
		hdr.bits.success = true;
		hdr.bits.size = o.writeSize();
		*reinterpret_cast<Header*>(o.ptr(0)) = hdr;
		if (dbg)
		{
			CZRPC_LOG(Log, CZRPC_LOGSTR_REPLY"size=%u, success",
				dbg->num, o.writeSize());
		}
		trp.send(o.extract());
	}
};

template <typename R>
struct Caller
{
    template <typename OBJ, typename F, typename P>
    static void doCall(OBJ& obj, F f, P&& params, Stream& out, Header hdr)
    {
        auto r = callMethod(obj, f, std::move(params));
        if (hdr.isGenericRPC())
            out << Any(r);
        else
            out << r;
    }
};

template <>
struct Caller<void>
{
    template <typename OBJ, typename F, typename P>
    static void doCall(OBJ& obj, F f, P&& params, Stream& out, Header hdr)
    {
        callMethod(obj, f, std::move(params));
        if (hdr.isGenericRPC())
            out << Any();
    }
};


template <bool ASYNC,typename R>
struct Dispatcher {};

// Handle synchronous RPCs
template <typename R>
struct Dispatcher<false, R>
{
	template <typename OBJ, typename F, typename P>
	static void impl(OBJ& obj, F f, P&& params, InProcessorData& out, Transport& trp, Header hdr, DebugInfo* dbg)
	{
#if CZRPC_CATCH_EXCEPTIONS
		try {
#endif
			Stream o;
			o << hdr; // Reserve space for the header
			Caller<R>::doCall(obj, std::move(f), std::move(params), o, hdr);
			Send::result(trp, hdr, o, dbg);
#if CZRPC_CATCH_EXCEPTIONS
		}
		catch (std::exception& e)
		{
			Send::error(trp, hdr, e.what(), dbg);
		}
#endif
	}
};

// For functions return std::future
template <typename R>
struct Dispatcher<true, R>
{
	template <typename OBJ, typename F, typename P>
	static void impl(OBJ& obj, F f, P&& params, InProcessorData& out, Transport& trp, Header hdr, DebugInfo* dbg)
	{
		using Traits = FunctionTraits<F>;
		auto resFt = callMethod(obj, f, std::move(params));
		out.pending([&](InProcessorData::PendingFutures& pending)
		{
			unsigned counter = pending.counter++;
			std::unique_ptr<DebugInfo> dbgptr;
			if (dbg)
				dbgptr = std::make_unique<DebugInfo>(*dbg);
			auto ft = then(std::move(resFt), [&out, &trp, hdr, counter, dbg=std::move(dbgptr)](std::future<typename Traits::return_type> ft)
			{
				processReady(out, trp, counter, hdr, std::move(ft), dbg.get());
			});

			pending.futures.insert(std::make_pair(counter, std::move(ft)));
		});
	}

	template<typename T>
	static void processReady(InProcessorData& out, Transport& trp, unsigned counter, Header hdr, std::future<T> ft, DebugInfo* dbg)
	{
		try
		{
			Stream o;
			o << hdr;
			auto r = ft.get();
			if (hdr.isGenericRPC())
				o << Any(r);
			else
				o << r;
			Send::result(trp, hdr, o, dbg);
		}
		catch (const std::exception& e)
		{
			Send::error(trp, hdr, e.what(), dbg);
		}

		// Delete previously finished futures, and prepare to delete this one.
		// We can't delete this one right here, because it will deadlock.
		out.pending([&](InProcessorData::PendingFutures& pending)
		{
			pending.done.clear();
			auto it = pending.futures.find(counter);
			if (it == pending.futures.end()) 
				return; // If the future is not found, it means we are shutting down
			//assert(it != pending.futures.end());
			pending.done.push_back(std::move(it->second));
			pending.futures.erase(it);
		});

	}
};

}

struct BaseInfo
{
	BaseInfo() {}
	virtual ~BaseInfo(){};
	std::string name;
};

class BaseTable
{
  public:
	explicit BaseTable(const char* name) : m_name(name) {}
	virtual ~BaseTable() {}
	bool isValid(uint32_t rpcid) const { return rpcid < m_rpcs.size(); }
  protected:
	std::string m_name;
	std::vector<std::unique_ptr<BaseInfo>> m_rpcs;
	std::vector<std::unique_ptr<BaseInfo>> m_controlrpcs;
};

template <typename T>
class TableImpl : public BaseTable
{
  public:
	using Type = T;

	struct Info : public BaseInfo
	{
		std::function<void(Type&, Stream& in, InProcessorData& out, Transport& trp, Header hdr, DebugInfo* dbg)> dispatcher;
	};

	explicit TableImpl(const char* name) : BaseTable(name) {}

	Info* getByName(const std::string& name)
	{
		for(auto&& info : m_rpcs)
		{
			if (info->name == name)
				return static_cast<Info*>(info.get());
		}
		return nullptr;
	}

	Info* getControlByName(const std::string& name)
	{
		for(auto&& info : m_controlrpcs)
		{
			if (info->name == name)
				return static_cast<Info*>(info.get());
		}
		return nullptr;
	}

	void registerGenericRPC()
	{
		// Generic RPC needs to have ID 0
		assert(m_rpcs.size() == 0);
		auto info = std::make_unique<Info>();
		info->name = "genericRPC";
		info->dispatcher = [this](Type& obj, Stream& in, InProcessorData& out, Transport& trp, Header hdr, DebugInfo* dbg) {
			assert(hdr.isGenericRPC());
			std::string name;
			in >> name;

			// Search first in user RPCs, for performance reasons, since those are called most often
			Info* info = getByName(name);
			if (!info)
				info = getControlByName(name);

			if (!info)
			{
				details::Send::error(trp, hdr, "Generic RPC not found", dbg);
				return;
			}

			info->dispatcher(obj, in, out, trp, hdr, dbg);
		};
		m_rpcs.push_back(std::move(info));

		// Register control RPCs
		registerControlRPC("__auth", &InProcessorData::auth);
		registerControlRPC("__getProperty", &InProcessorData::getProperty);
		registerControlRPC("__setProperty", &InProcessorData::setProperty);
	}

	template <typename F>
	void registerRPC(uint32_t rpcid, const char* name, F f)
	{
		assert(rpcid == m_rpcs.size());

		// Make sure there are no two RPCs with the same name
		// NOTE: For the user RPCs alone, this would not be necessary, since the enums
		// would not allow two RPCs with the same name. But we also need to make sure user RPC names
		// don't collide with the control RPCs
		//
		assert(getByName(name) == nullptr);
		assert(getControlByName(name) == nullptr);

		auto info = std::make_unique<Info>();
		info->name = name;
		info->dispatcher = [this,f,info=info.get()](Type& obj, Stream& in, InProcessorData& out, Transport& trp, Header hdr, DebugInfo* dbg) {
			using Traits = FunctionTraits<F>;
			typename Traits::param_tuple params;

			if(dbg && hdr.isGenericRPC())
			{
				CZRPC_LOG(Log, CZRPC_LOGSTR_RECEIVE"genericRPC->%s::%s", dbg->num, m_name.c_str(), info->name.c_str());
			}

			if (!out.authPassed)
			{
				if (dbg)
				{
					CZRPC_LOG(Log, CZRPC_LOGSTR_RECEIVE"No authentication. Closing.", dbg->num);
				}
				trp.close();
				return;
			}

			if (hdr.isGenericRPC())
			{
				std::vector<Any> a;
				in >> a;
				if (!toTuple(a, params))
				{
					// Invalid parameters supplied, or the RPC function signature itself can't be used for
					// generic RPCs, since the parameter types it uses can't be converted to/from cz::rpc::Any
					details::Send::error(trp, hdr, "Invalid parameters for generic RPC", dbg);
					return;
				}
			}
			else
			{
				in >> params;
			}

			using R = typename Traits::return_type;
			details::Dispatcher<Traits::isasync, R>::impl(obj, f, std::move(params), out, trp, hdr, dbg);
		};
		m_rpcs.push_back(std::move(info));
	}

	template<typename F>
	void registerControlRPC(const char* name, F f)
	{
		assert(getByName(name) == nullptr);
		assert(getControlByName(name) == nullptr);

		auto info = std::make_unique<Info>();
		info->name = name;
		info->dispatcher = [this, f, info=info.get()](Type& obj, Stream& in, InProcessorData& out, Transport& trp, Header hdr, DebugInfo* dbg) {
			using Traits = FunctionTraits<F>;
			typename Traits::param_tuple params;
			// All control RPCs are generic (and only generic)
			assert(hdr.isGenericRPC());
			
			if(dbg)
			{
				CZRPC_LOG(Log, CZRPC_LOGSTR_RECEIVE"genericRPC->%s::%s", dbg->num, m_name.c_str(), info->name.c_str());
			}

			if (!out.authPassed && info->name!="__auth")
			{
				if (dbg)
				{
					CZRPC_LOG(Log, CZRPC_LOGSTR_RECEIVE"No authentication. Closing.", dbg->num);
				}
				trp.close();
				return;
			}

			std::vector<Any> a;
			in >> a;
			if (!toTuple(a, params))
			{
				details::Send::error(trp, hdr, "Invalid parameters for generic RPC", dbg);
				return;
			}

			using R = typename Traits::return_type;
			// Forcing all control RPCs to return Any simplifies things, since they are meant to be used with
			// generic RPC calls anyway (and those always return Any)
			static_assert(std::is_same<R, Any>::value, "control RPC function needs to return Any");
			Stream o;
			o << hdr; // reserve space for header
			o << callMethod(out, f, std::move(params));
			details::Send::result(trp, hdr, o, dbg);
		};
		m_controlrpcs.push_back(std::move(info));
	}
};

template <typename T>
class Table : public TableImpl<T>
{
public:
	// Using std::is_pointer has a hack to cause this assert to always fail if we try to instantiate
	// an instace of a non specialized Table
	static_assert(std::is_pointer<T>::value, "RPC Table not specified for the type.");
};

}  // namespace rpc
}  // namespace cz
