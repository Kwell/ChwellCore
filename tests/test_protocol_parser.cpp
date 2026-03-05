#include <gtest/gtest.h>

#include "chwell/protocol/message.h"
#include "chwell/protocol/parser.h"

using namespace chwell;

TEST(ProtocolParserTest, SerializeAndDeserializeRoundtrip) {
    protocol::Message msg_out(100, std::string("hello"));

    auto data = protocol::serialize(msg_out);

    protocol::Message msg_in;
    ASSERT_TRUE(protocol::deserialize(data, msg_in));
    EXPECT_EQ(msg_out.cmd, msg_in.cmd);
    std::string body_in(msg_in.body.begin(), msg_in.body.end());
    EXPECT_EQ("hello", body_in);
}

TEST(ProtocolParserTest, HandlesStickyAndPartialPackets) {
    protocol::Message m1(1, std::string("foo"));
    protocol::Message m2(2, std::string("barbaz"));

    auto d1 = protocol::serialize(m1);
    auto d2 = protocol::serialize(m2);

    // 粘包：两条消息连在一起
    std::vector<char> combined;
    combined.reserve(d1.size() + d2.size());
    combined.insert(combined.end(), d1.begin(), d1.end());
    combined.insert(combined.end(), d2.begin(), d2.end());

    protocol::Parser parser;
    auto messages = parser.feed(combined);
    ASSERT_EQ(2u, messages.size());
    EXPECT_EQ(1u, messages[0].cmd);
    EXPECT_EQ(2u, messages[1].cmd);

    // 部分包：先一半，再一半
    protocol::Parser parser2;
    std::vector<char> first_half(d1.begin(), d1.begin() + d1.size() / 2);
    std::vector<char> second_half(d1.begin() + d1.size() / 2, d1.end());

    auto m_first = parser2.feed(first_half);
    EXPECT_TRUE(m_first.empty());

    auto m_second = parser2.feed(second_half);
    ASSERT_EQ(1u, m_second.size());
    EXPECT_EQ(1u, m_second[0].cmd);
}

