#pragma once

// SBE framing over sequencer payloads (docs/spec.md §4).
//
// Every payload -- braft log entry, journal input, journal output,
// snapshot record -- is one SBE message: the standard 8-byte
// messageHeader, then the message body. The generated codecs
// (generated/exchange/*.h, from schema/exchange.xml) are casts over
// the buffer; this header adds the three things they leave to the
// caller: reading the header before choosing a decoder, refusing what
// this binary cannot interpret, and fixed-width char arrays without
// reading past a shorter string.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

#include <exchange/MessageHeader.h>

namespace exchange::wire {

using Bytes = std::span<const std::byte>;

struct Header {
  std::uint16_t blockLength;
  std::uint16_t templateId;
  std::uint16_t schemaId;
  std::uint16_t version;
};

inline constexpr std::size_t kHeaderLength = MessageHeader::encodedLength();

// The header, if there is room for one. Nothing else is validated.
inline std::optional<Header> peekHeader(Bytes bytes) noexcept {
  if (bytes.size() < kHeaderLength) {
    return std::nullopt;
  }
  MessageHeader header;
  // SBE's decoders take a mutable buffer; nothing here writes through it.
  header.wrap(const_cast<char*>(reinterpret_cast<const char*>(bytes.data())), 0, 0, bytes.size());
  return Header{header.blockLength(), header.templateId(), header.schemaId(), header.version()};
}

// Why a payload could not be decoded as `Msg`. A replicated state
// machine must not guess: a payload it cannot interpret identically to
// every other replica is rejected, deterministically, with a reason.
enum class Rejection : std::uint8_t {
  TooShort,        // no room for the header, or for the block the header announces
  WrongSchema,     // schemaId is not this schema's
  WrongTemplate,   // a different message type; try another decoder
  FutureVersion,   // written by a newer schema than this binary was built with
};

template <class Msg>
struct Decoded {
  std::optional<Msg> message;
  Rejection rejection = Rejection::TooShort;  // meaningful only when !message
  explicit operator bool() const noexcept { return message.has_value(); }
  Msg& operator*() noexcept { return *message; }
  Msg* operator->() noexcept { return &*message; }
};

// Wrap `bytes` as `Msg` for decoding, after checking the header.
//
// Older versions decode through SBE's acting-version rules. Newer ones
// are refused: a replica on old code replaying a journal written by
// new code must fail loudly rather than silently apply what it half
// understands (sequencer spec §8.3 guarantees old-journal-under-new-
// binary, not the reverse).
template <class Msg>
Decoded<Msg> decode(Bytes bytes) noexcept {
  const auto header = peekHeader(bytes);
  if (!header) {
    return {std::nullopt, Rejection::TooShort};
  }
  if (header->schemaId != Msg::sbeSchemaId()) {
    return {std::nullopt, Rejection::WrongSchema};
  }
  if (header->templateId != Msg::sbeTemplateId()) {
    return {std::nullopt, Rejection::WrongTemplate};
  }
  if (header->version > Msg::sbeSchemaVersion()) {
    return {std::nullopt, Rejection::FutureVersion};
  }
  if (bytes.size() < kHeaderLength + header->blockLength) {
    return {std::nullopt, Rejection::TooShort};
  }
  Msg msg;
  msg.wrapForDecode(const_cast<char*>(reinterpret_cast<const char*>(bytes.data())), kHeaderLength,
                    header->blockLength, header->version, bytes.size());
  return {std::move(msg), Rejection::TooShort};
}

// Wrap `buffer` as `Msg` for encoding, header written. The caller
// fills fields, then reads encodedLength() for what to ship.
template <class Msg>
Msg encode(std::span<std::byte> buffer) {
  Msg msg;
  msg.wrapAndApplyHeader(reinterpret_cast<char*>(buffer.data()), 0, buffer.size());
  return msg;
}

// Header plus body, i.e. the payload to propose / emit.
template <class Msg>
std::size_t encodedLength(const Msg& msg) noexcept {
  return kHeaderLength + msg.encodedLength();
}

// Fixed-width char fields. SBE's put<Field>(const char*) copies exactly
// N bytes, so a shorter string must be padded first; and its
// get<Field>AsString() allocates, which the apply path may not.
template <std::size_t N>
std::array<char, N> fixed(std::string_view text) noexcept {
  std::array<char, N> out{};
  const std::size_t n = text.size() < N ? text.size() : N;
  std::memcpy(out.data(), text.data(), n);
  return out;
}

// The string in a fixed-width field: trailing NULs trimmed, no copy.
inline std::string_view view(const char* field, std::size_t width) noexcept {
  std::size_t n = 0;
  while (n < width && field[n] != '\0') {
    ++n;
  }
  return {field, n};
}

inline bool fits(std::string_view text, std::size_t width) noexcept { return text.size() <= width; }

}  // namespace exchange::wire
