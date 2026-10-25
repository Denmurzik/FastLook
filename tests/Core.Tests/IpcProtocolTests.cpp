#include <gtest/gtest.h>

#include <string>

#include "Ipc/Framing.h"
#include "Ipc/Protocol.h"

using namespace fastlook::core;
using namespace fastlook::core::ipc;

TEST(IpcProtocol, ShowRoundTripsUnicodeAndOptionalRect)
{
    Message m{.type = MsgType::Toggle, .id = 7};
    m.show = {.explorerHwnd = 0x1234,
              .monitor = 99,
              .items = {{L"C:\\Тест\\файл 1.txt"}, {L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", true}},
              .index = 1,
              .listComplete = false,
              .navMode = NavMode::Folder,
              .iconRect = RectI{10, 20, 48, 48}};
    auto p = Parse(Serialize(m));
    ASSERT_TRUE(p);
    EXPECT_EQ(*p, m);
    m.show.iconRect.reset();
    EXPECT_EQ(*Parse(Serialize(m)), m);
    EXPECT_EQ(Serialize(m).find("iconRect"), std::string::npos);
}

TEST(IpcProtocol, ItemsPongAckEventRoundTrip)
{
    Message show{.type = MsgType::Show, .id = 1};
    show.show.explorerHwnd = 0xFFFFFFFFFFFFFFFFull;
    show.show.isDesktop = true;
    show.show.items = {{L"C:\\a.txt"}};

    Message items{.type = MsgType::Items, .id = 9};
    items.items = {.items = {{L"D:\\b.png"}, {L"::{GUID}", true}}, .index = 3, .navMode = NavMode::Selection};

    Message openSettings{.type = MsgType::OpenSettings, .id = 12};
    openSettings.page = "general";

    Message ack{.type = MsgType::Ack, .id = 13, .replyTo = 7};
    ack.shown = true;

    Message event{.type = MsgType::Event, .id = 14};
    event.eventName = "pinned";

    for (Message const& m : {show, items, openSettings, ack, event,
                             Message{.type = MsgType::Ping, .id = 5},
                             Message{.type = MsgType::Pong, .id = 11, .replyTo = 10},
                             Message{.type = MsgType::SettingsChanged, .id = 15},
                             Message{.type = MsgType::Shutdown, .id = 16}})
    {
        auto p = Parse(Serialize(m));
        ASSERT_TRUE(p) << Serialize(m);
        EXPECT_EQ(*p, m) << Serialize(m);
    }
}

TEST(IpcProtocol, WireFormatMatchesSpec)
{
    Message m{.type = MsgType::Show, .id = 7};
    m.show = {.explorerHwnd = 1234,
              .monitor = 5678,
              .items = {{L"C:\\a.txt"}},
              .index = 0,
              .listComplete = false,
              .navMode = NavMode::Folder,
              .iconRect = RectI{10, 20, 48, 48}};
    EXPECT_EQ(Serialize(m),
              R"({"v":1,"id":7,"type":"show","explorerHwnd":1234,"isDesktop":false,"monitor":5678,"index":0,)"
              R"("listComplete":false,"navMode":"folder","iconRect":[10,20,48,48],"items":[{"path":"C:\\a.txt","virtual":false}]})");
    EXPECT_EQ(Serialize(Message{.type = MsgType::Pong, .id = 11, .replyTo = 10}),
              R"({"v":1,"id":11,"re":10,"type":"pong"})");
    Message ack{.type = MsgType::Ack, .id = 13, .replyTo = 7};
    ack.shown = true;
    EXPECT_EQ(Serialize(ack), R"({"v":1,"id":13,"re":7,"type":"ack","shown":true})");
    Message ev{.type = MsgType::Event, .id = 14};
    ev.eventName = "pinned";
    EXPECT_EQ(Serialize(ev), R"({"v":1,"id":14,"type":"event","name":"pinned"})");
}

TEST(IpcProtocol, ParsesPathsAsUtf8)
{
    auto p = Parse("{\"v\":1,\"id\":1,\"type\":\"show\",\"items\":[{\"path\":\"C:\\\\\xD0\xA2\xD0\xB5\xD1\x81\xD1\x82\",\"virtual\":false}]}");
    ASSERT_TRUE(p);
    ASSERT_EQ(p->show.items.size(), 1u);
    EXPECT_EQ(p->show.items[0].path, L"C:\\Тест");
}

TEST(IpcProtocol, RejectsWrongVersionAndGarbage)
{
    EXPECT_FALSE(Parse(R"({"v":2,"id":1,"type":"ping"})"));
    EXPECT_FALSE(Parse(R"({"id":1,"type":"ping"})"));
    EXPECT_FALSE(Parse("{"));
    EXPECT_FALSE(Parse("[]"));
    EXPECT_FALSE(Parse(R"({"v":1,"id":1})"));
    EXPECT_FALSE(Parse(R"({"v":1,"type":"dance"})"));
    EXPECT_FALSE(Parse(R"({"v":1,"type":"show","navMode":"sideways"})"));
    EXPECT_FALSE(Parse(R"({"v":1,"type":"show","iconRect":[1,2,3]})"));
    EXPECT_FALSE(Parse(R"({"v":1,"type":"show","items":[{"virtual":false}]})"));
}

TEST(IpcFraming, SplitAndCoalescedFrames)
{
    std::string a = Frame("one"), b = Frame("two");
    FrameReader r;
    for (char c : a)
    {
        r.Feed({&c, 1});
    }
    EXPECT_EQ(r.Next(), "one");
    EXPECT_FALSE(r.Next());
    auto both = Frame("x") + b;
    r.Feed(both);
    EXPECT_EQ(r.Next(), "x");
    EXPECT_EQ(r.Next(), "two");
    EXPECT_FALSE(r.Next());
}

TEST(IpcFraming, FrameIsLittleEndianLengthPlusPayload)
{
    EXPECT_EQ(Frame("abc"), std::string("\x03\x00\x00\x00" "abc", 7));
    EXPECT_EQ(Frame(""), std::string("\0\0\0\0", 4));
}

TEST(IpcFraming, EmptyPayloadFrame)
{
    FrameReader r;
    auto f = Frame("");
    r.Feed(f);
    EXPECT_EQ(r.Next(), "");
    EXPECT_FALSE(r.Failed());
}

TEST(IpcFraming, OversizeFails)
{
    FrameReader r;
    uint32_t n = kMaxFrameBytes + 1;
    r.Feed({reinterpret_cast<char*>(&n), 4});
    EXPECT_TRUE(r.Failed());
    EXPECT_FALSE(r.Next());
}

TEST(IpcFraming, MaxSizeHeaderIsAccepted)
{
    FrameReader r;
    uint32_t n = kMaxFrameBytes;
    r.Feed({reinterpret_cast<char*>(&n), 4});
    EXPECT_FALSE(r.Failed());
    EXPECT_FALSE(r.Next());
}

TEST(IpcFraming, OversizeHeaderAfterValidFrameFails)
{
    FrameReader r;
    uint32_t n = kMaxFrameBytes + 1;
    auto data = Frame("ok") + std::string(reinterpret_cast<char*>(&n), 4);
    r.Feed(data);
    EXPECT_EQ(r.Next(), "ok");
    EXPECT_TRUE(r.Failed());
}
