/* Generated SBE (Simple Binary Encoding) message codec */
#ifndef _EXCHANGE_REJECTREASON_CXX_H_
#define _EXCHANGE_REJECTREASON_CXX_H_

#if !defined(__STDC_LIMIT_MACROS)
#  define __STDC_LIMIT_MACROS 1
#endif

#include <cstdint>
#include <iomanip>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <sstream>
#include <string>

#define SBE_NULLVALUE_INT8 (std::numeric_limits<std::int8_t>::min)()
#define SBE_NULLVALUE_INT16 (std::numeric_limits<std::int16_t>::min)()
#define SBE_NULLVALUE_INT32 (std::numeric_limits<std::int32_t>::min)()
#define SBE_NULLVALUE_INT64 (std::numeric_limits<std::int64_t>::min)()
#define SBE_NULLVALUE_UINT8 (std::numeric_limits<std::uint8_t>::max)()
#define SBE_NULLVALUE_UINT16 (std::numeric_limits<std::uint16_t>::max)()
#define SBE_NULLVALUE_UINT32 (std::numeric_limits<std::uint32_t>::max)()
#define SBE_NULLVALUE_UINT64 (std::numeric_limits<std::uint64_t>::max)()

namespace exchange {

class RejectReason
{
public:
    enum Value
    {
        MalformedMessage = static_cast<std::uint8_t>(1),
        UnknownInstrument = static_cast<std::uint8_t>(2),
        InstrumentExists = static_cast<std::uint8_t>(3),
        InvalidInstrument = static_cast<std::uint8_t>(4),
        PriceNotOnTick = static_cast<std::uint8_t>(5),
        QuantityNotOnLot = static_cast<std::uint8_t>(6),
        QuantityTooLarge = static_cast<std::uint8_t>(7),
        DuplicateClOrdId = static_cast<std::uint8_t>(8),
        UnknownOrder = static_cast<std::uint8_t>(9),
        NotOrderOwner = static_cast<std::uint8_t>(10),
        UnsupportedOrdType = static_cast<std::uint8_t>(11),
        UnsupportedTimeInForce = static_cast<std::uint8_t>(12),
        SideMismatch = static_cast<std::uint8_t>(13),
        NotAuthorized = static_cast<std::uint8_t>(14),
        ReplaceQuantityBelowFilled = static_cast<std::uint8_t>(15),
        SymbolMismatch = static_cast<std::uint8_t>(16),
        NULL_VALUE = static_cast<std::uint8_t>(255)
    };

    static RejectReason::Value get(const std::uint8_t value)
    {
        switch (value)
        {
            case static_cast<std::uint8_t>(1): return MalformedMessage;
            case static_cast<std::uint8_t>(2): return UnknownInstrument;
            case static_cast<std::uint8_t>(3): return InstrumentExists;
            case static_cast<std::uint8_t>(4): return InvalidInstrument;
            case static_cast<std::uint8_t>(5): return PriceNotOnTick;
            case static_cast<std::uint8_t>(6): return QuantityNotOnLot;
            case static_cast<std::uint8_t>(7): return QuantityTooLarge;
            case static_cast<std::uint8_t>(8): return DuplicateClOrdId;
            case static_cast<std::uint8_t>(9): return UnknownOrder;
            case static_cast<std::uint8_t>(10): return NotOrderOwner;
            case static_cast<std::uint8_t>(11): return UnsupportedOrdType;
            case static_cast<std::uint8_t>(12): return UnsupportedTimeInForce;
            case static_cast<std::uint8_t>(13): return SideMismatch;
            case static_cast<std::uint8_t>(14): return NotAuthorized;
            case static_cast<std::uint8_t>(15): return ReplaceQuantityBelowFilled;
            case static_cast<std::uint8_t>(16): return SymbolMismatch;
            case static_cast<std::uint8_t>(255): return NULL_VALUE;
        }

        throw std::runtime_error("unknown value for enum RejectReason [E103]");
    }

    static const char *c_str(const RejectReason::Value value)
    {
        switch (value)
        {
            case MalformedMessage: return "MalformedMessage";
            case UnknownInstrument: return "UnknownInstrument";
            case InstrumentExists: return "InstrumentExists";
            case InvalidInstrument: return "InvalidInstrument";
            case PriceNotOnTick: return "PriceNotOnTick";
            case QuantityNotOnLot: return "QuantityNotOnLot";
            case QuantityTooLarge: return "QuantityTooLarge";
            case DuplicateClOrdId: return "DuplicateClOrdId";
            case UnknownOrder: return "UnknownOrder";
            case NotOrderOwner: return "NotOrderOwner";
            case UnsupportedOrdType: return "UnsupportedOrdType";
            case UnsupportedTimeInForce: return "UnsupportedTimeInForce";
            case SideMismatch: return "SideMismatch";
            case NotAuthorized: return "NotAuthorized";
            case ReplaceQuantityBelowFilled: return "ReplaceQuantityBelowFilled";
            case SymbolMismatch: return "SymbolMismatch";
            case NULL_VALUE: return "NULL_VALUE";
        }

        throw std::runtime_error("unknown value for enum RejectReason [E103]:");
    }

    template<typename CharT, typename Traits>
    friend std::basic_ostream<CharT, Traits> & operator << (
        std::basic_ostream<CharT, Traits> &os, RejectReason::Value m)
    {
        return os << RejectReason::c_str(m);
    }
};

}

#endif
