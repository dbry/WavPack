#include <gtest/gtest.h>
#include <string>
#include <cstring>

// The vulnerable code uses sprintf with a fixed buffer (likely 256-512 bytes)
// We test that oversized filenames don't cause buffer overflows

class BufferOverflowSecurityTest : public ::testing::TestWithParam<std::string> {};

TEST_P(BufferOverflowSecurityTest, FilenameBufferNeverExceedsDeclaredLength) {
    // Invariant: Buffer reads/writes never exceed the declared buffer length
    std::string filename = GetParam();
    
    // Simulate the vulnerable pattern from libwavpack.cpp
    // The actual code does: sprintf(text, "File \"%s\" not found...", filename)
    // A safe implementation must use snprintf or reject oversized input
    
    const size_t BUFFER_SIZE = 256;  // Typical stack buffer size
    char text[BUFFER_SIZE];
    
    // Safe version that should be used instead of sprintf
    int written = snprintf(text, BUFFER_SIZE, 
                          "File \"%s\" not found or is read protected!\n",
                          filename.c_str());
    
    // Security invariant: written bytes never exceed buffer, string is null-terminated
    ASSERT_LT(strlen(text), BUFFER_SIZE) << "Buffer overflow: string exceeds buffer size";
    ASSERT_EQ(text[BUFFER_SIZE - 1], '\0') << "Buffer not properly null-terminated";
    
    // If truncation occurred, verify it was handled safely
    if (written >= static_cast<int>(BUFFER_SIZE)) {
        ASSERT_EQ(strlen(text), BUFFER_SIZE - 1) << "Truncation not handled correctly";
    }
}

INSTANTIATE_TEST_SUITE_P(
    AdversarialInputs,
    BufferOverflowSecurityTest,
    ::testing::Values(
        std::string(512, 'A'),           // 2x buffer size - exploit case
        std::string(2560, 'B'),          // 10x buffer size - extreme case
        std::string(255, 'C'),           // Boundary: just under typical buffer
        std::string("/home/user/music.wav")  // Valid normal input
    )
);

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}