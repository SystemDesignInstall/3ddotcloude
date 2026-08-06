// frame_echo: a tiny helper driven as a child process by the framing tests.
// It reads [u32 LE length][payload] frames from stdin and echoes each frame
// verbatim to stdout (worker-protocol §1), so tests can validate the framing
// over real pipes without a Python dependency.

#include <cstdint>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#define SPATIAL_READ _read
#define SPATIAL_WRITE _write
#else
#include <unistd.h>
#define SPATIAL_READ ::read
#define SPATIAL_WRITE ::write
#endif

namespace {

bool ReadExact(std::uint8_t* out, std::size_t n) {
  std::size_t got = 0;
  while (got < n) {
    const int r = SPATIAL_READ(0, out + got, static_cast<unsigned>(n - got));
    if (r <= 0) {
      return false;
    }
    got += static_cast<std::size_t>(r);
  }
  return true;
}

bool WriteExact(const std::uint8_t* data, std::size_t n) {
  std::size_t sent = 0;
  while (sent < n) {
    const int w =
        SPATIAL_WRITE(1, data + sent, static_cast<unsigned>(n - sent));
    if (w <= 0) {
      return false;
    }
    sent += static_cast<std::size_t>(w);
  }
  return true;
}

}  // namespace

int main() {
#if defined(_WIN32)
  // The protocol is byte-exact: the CRT must not translate newlines or treat
  // 0x1A (Ctrl-Z) as EOF on the inherited pipe handles (worker-protocol §1).
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stderr), _O_BINARY);
#endif
  for (;;) {
    std::uint8_t prefix[4] = {0, 0, 0, 0};
    if (!ReadExact(prefix, 4)) {
      return 0;  // clean EOF
    }
    const std::uint32_t length =
        static_cast<std::uint32_t>(prefix[0]) |
        (static_cast<std::uint32_t>(prefix[1]) << 8) |
        (static_cast<std::uint32_t>(prefix[2]) << 16) |
        (static_cast<std::uint32_t>(prefix[3]) << 24);
    std::vector<std::uint8_t> body(length);
    if (!ReadExact(body.data(), length)) {
      return 1;  // truncated frame
    }
    if (!WriteExact(prefix, 4) || !WriteExact(body.data(), length)) {
      return 1;
    }
  }
}
