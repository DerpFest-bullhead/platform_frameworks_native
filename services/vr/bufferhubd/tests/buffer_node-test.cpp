#include <errno.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <private/dvr/buffer_node.h>

namespace android {
namespace dvr {

namespace {

const uint32_t kWidth = 640;
const uint32_t kHeight = 480;
const uint32_t kLayerCount = 1;
const uint32_t kFormat = 1;
const uint64_t kUsage = 0;
const size_t kUserMetadataSize = 0;

class BufferNodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    buffer_node = new BufferNode(kWidth, kHeight, kLayerCount, kFormat, kUsage,
                                 kUserMetadataSize);
    ASSERT_TRUE(buffer_node->IsValid());
  }

  void TearDown() override {
    if (buffer_node != nullptr) {
      delete buffer_node;
    }
  }

  BufferNode* buffer_node = nullptr;
};

TEST_F(BufferNodeTest, TestCreateBufferNode) {
  EXPECT_EQ(buffer_node->user_metadata_size(), kUserMetadataSize);
}

TEST_F(BufferNodeTest, TestAddNewActiveClientsBitToMask_twoNewClients) {
  uint64_t new_buffer_state_bit_1 = buffer_node->AddNewActiveClientsBitToMask();
  EXPECT_EQ(buffer_node->GetActiveClientsBitMask(), new_buffer_state_bit_1);

  // Request and add a new buffer_state_bit again.
  // Active clients bit mask should be the union of the two new
  // buffer_state_bits.
  uint64_t new_buffer_state_bit_2 = buffer_node->AddNewActiveClientsBitToMask();
  EXPECT_EQ(buffer_node->GetActiveClientsBitMask(),
            new_buffer_state_bit_1 | new_buffer_state_bit_2);
}

TEST_F(BufferNodeTest, TestAddNewActiveClientsBitToMask_32NewClients) {
  uint64_t new_buffer_state_bit = 0ULL;
  uint64_t current_mask = 0ULL;
  uint64_t expected_mask = 0ULL;

  for (int i = 0; i < 64; ++i) {
    new_buffer_state_bit = buffer_node->AddNewActiveClientsBitToMask();
    EXPECT_NE(new_buffer_state_bit, 0);
    EXPECT_FALSE(new_buffer_state_bit & current_mask);
    expected_mask = current_mask | new_buffer_state_bit;
    current_mask = buffer_node->GetActiveClientsBitMask();
    EXPECT_EQ(current_mask, expected_mask);
  }

  // Method should fail upon requesting for more than maximum allowable clients.
  new_buffer_state_bit = buffer_node->AddNewActiveClientsBitToMask();
  EXPECT_EQ(new_buffer_state_bit, 0ULL);
  EXPECT_EQ(errno, E2BIG);
}

TEST_F(BufferNodeTest, TestRemoveActiveClientsBitFromMask) {
  buffer_node->AddNewActiveClientsBitToMask();
  uint64_t current_mask = buffer_node->GetActiveClientsBitMask();
  uint64_t new_buffer_state_bit = buffer_node->AddNewActiveClientsBitToMask();
  EXPECT_NE(buffer_node->GetActiveClientsBitMask(), current_mask);

  buffer_node->RemoveClientsBitFromMask(new_buffer_state_bit);
  EXPECT_EQ(buffer_node->GetActiveClientsBitMask(), current_mask);

  // Remove the test_mask again to the active client bit mask should not modify
  // the value of active clients bit mask.
  buffer_node->RemoveClientsBitFromMask(new_buffer_state_bit);
  EXPECT_EQ(buffer_node->GetActiveClientsBitMask(), current_mask);
}

}  // namespace

}  // namespace dvr
}  // namespace android
