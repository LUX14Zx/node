#include "crypto/crypto_clienthello-inl.h"
#include "gtest/gtest.h"

// If the test is being compiled with an address sanitizer enabled, it should
// catch the memory violation, so do not use a guard page.
#ifdef __SANITIZE_ADDRESS__
#define NO_GUARD_PAGE
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define NO_GUARD_PAGE
#endif
#endif

// If the test is running without an address sanitizer, see if we can use
// mprotect() or VirtualProtect() to cause a segmentation fault when spatial
// safety is violated.
#if !defined(NO_GUARD_PAGE)
#ifdef __linux__
#include <sys/mman.h>
#include <unistd.h>
#if defined(_SC_PAGE_SIZE) && defined(PROT_NONE) && defined(PROT_READ) &&      \
    defined(PROT_WRITE)
#define USE_MPROTECT
#endif
#elif defined(_WIN32) && defined(_MSC_VER)
#include <Windows.h>
#include <memoryapi.h>
#define USE_VIRTUALPROTECT
#endif
#endif

template <size_t N>
class OverrunGuardedBuffer {
 public:
  OverrunGuardedBuffer() {
#ifdef USE_MPROTECT
    // Place the packet right before a guard page, which, when accessed, causes
    // a segmentation fault.
    int page = sysconf(_SC_PAGE_SIZE);
    EXPECT_GE(page, static_cast<int>(N));
    alloc_base = static_cast<uint8_t*>(aligned_alloc(page, 2 * page));
    EXPECT_NE(alloc_base, nullptr);
    uint8_t* second_page = alloc_base + page;
    EXPECT_EQ(mprotect(second_page, page, PROT_NONE), 0);
    data_base = second_page - N;
#elif defined(USE_VIRTUALPROTECT)
    // On Windows, it works almost the same way.
    SYSTEM_INFO system_info;
    GetSystemInfo(&system_info);
    DWORD page = system_info.dwPageSize;
    alloc_base = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 2 * page, MEM_COMMIT, PAGE_READWRITE));
    EXPECT_NE(alloc_base, nullptr);
    uint8_t* second_page = alloc_base + page;
    DWORD old_prot;
    EXPECT_NE(VirtualProtect(second_page, page, PAGE_NOACCESS, &old_prot), 0);
    EXPECT_EQ(old_prot, PAGE_READWRITE);
    data_base = second_page - N;
#else
    // Place the packet in a regular allocated buffer. The bug causes undefined
    // behavior, which might crash the process, and when it does not, address
    // sanitizers and valgrind will catch it.
    alloc_base = static_cast<uint8_t*>(malloc(N));
    EXPECT_NE(alloc_base, nullptr);
    data_base = alloc_base;
#endif
  }

  OverrunGuardedBuffer(const OverrunGuardedBuffer& other) = delete;
  OverrunGuardedBuffer& operator=(const OverrunGuardedBuffer& other) = delete;

  ~OverrunGuardedBuffer() {
#ifdef USE_VIRTUALPROTECT
    SYSTEM_INFO system_info;
    GetSystemInfo(&system_info);
    DWORD page = system_info.dwPageSize;
    VirtualFree(alloc_base, 2 * system_info.dwPageSize, MEM_RELEASE);
#else
#ifdef USE_MPROTECT
    // Revert page protection such that the memory can be free()'d.
    int page = sysconf(_SC_PAGE_SIZE);
    EXPECT_GE(page, static_cast<int>(N));
    uint8_t* second_page = alloc_base + page;
    EXPECT_EQ(mprotect(second_page, page, PROT_READ | PROT_WRITE), 0);
#endif
    free(alloc_base);
#endif
  }

  uint8_t* data() {
    return data_base;
  }

 private:
  uint8_t* alloc_base;
  uint8_t* data_base;
};

// Test that ClientHelloParser::ParseHeader() does not blindly trust the client
// to send a valid frame length and subsequently does not read out-of-bounds.
TEST(NodeCrypto, ClientHelloParserParseHeaderOutOfBoundsRead) {
  using node::crypto::ClientHelloParser;

  // This is the simplest packet triggering the bug.
  const uint8_t packet[] = {0x16, 0x03, 0x01, 0x00, 0x00};
  OverrunGuardedBuffer<sizeof(packet)> buffer;
  memcpy(buffer.data(), packet, sizeof(packet));

  // Let the ClientHelloParser parse the packet. This should not lead to a
  // segmentation fault or to undefined behavior.
  node::crypto::ClientHelloParser parser;
  bool end_cb_called = false;
  parser.Start([](void* arg, auto hello) { GTEST_FAIL(); },
               [](void* arg) {
                 bool* end_cb_called = static_cast<bool*>(arg);
                 EXPECT_FALSE(*end_cb_called);
                 *end_cb_called = true;
               },
               &end_cb_called);
  parser.Parse(buffer.data(), sizeof(packet));
  EXPECT_TRUE(end_cb_called);
}
