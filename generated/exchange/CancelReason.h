/* Generated SBE (Simple Binary Encoding) message codec */
#ifndef _EXCHANGE_CANCELREASON_CXX_H_
#define _EXCHANGE_CANCELREASON_CXX_H_

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

class CancelReason
{
public:
    enum Value
    {
        Requested = static_cast<std::uint8_t>(1),
        ImmediateOrCancelRemainder = static_cast<std::uint8_t>(2),
        FillOrKillUnfilled = static_cast<std::uint8_t>(3),
        Replaced = static_cast<std::uint8_t>(4),
        NULL_VALUE = static_cast<std::uint8_t>(255)
    };

    static CancelReason::Value get(const std::uint8_t value)
    {
        switch (value)
        {
            case static_cast<std::uint8_t>(1): return Requested;
            case static_cast<std::uint8_t>(2): return ImmediateOrCancelRemainder;
            case static_cast<std::uint8_t>(3): return FillOrKillUnfilled;
            case static_cast<std::uint8_t>(4): return Replaced;
            case static_cast<std::uint8_t>(255): return NULL_VALUE;
        }

        throw std::runtime_error("unknown value for enum CancelReason [E103]");
    }

    static const char *c_str(const CancelReason::Value value)
    {
        switch (value)
        {
            case Requested: return "Requested";
            case ImmediateOrCancelRemainder: return "ImmediateOrCancelRemainder";
            case FillOrKillUnfilled: return "FillOrKillUnfilled";
            case Replaced: return "Replaced";
            case NULL_VALUE: return "NULL_VALUE";
        }

        throw std::runtime_error("unknown value for enum CancelReason [E103]:");
    }

    template<typename CharT, typename Traits>
    friend std::basic_ostream<CharT, Traits> & operator << (
        std::basic_ostream<CharT, Traits> &os, CancelReason::Value m)
    {
        return os << CancelReason::c_str(m);
    }
};

}

#endif
