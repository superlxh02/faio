#include <gtest/gtest.h>

bool module_io_public_header_smoke();
bool module_fs_public_header_smoke();
bool module_net_public_header_smoke();
bool module_protocol_header_smoke();

// 每个实现位于独立 TU，不能靠另一个模块的先行 include 补齐入口依赖。
TEST(ModuleLayoutTest, PublicIoEntryPreservesBufferOwnership) {
  EXPECT_TRUE(module_io_public_header_smoke());
}

TEST(ModuleLayoutTest, PublicFsEntryPreservesOptionAndOffsetContracts) {
  EXPECT_TRUE(module_fs_public_header_smoke());
}

TEST(ModuleLayoutTest, PublicNetEntryPreservesEndpointValues) {
  EXPECT_TRUE(module_net_public_header_smoke());
}

TEST(ModuleLayoutTest, GenericProtocolsPreserveTokenAndHandleWidths) {
  EXPECT_TRUE(module_protocol_header_smoke());
}
