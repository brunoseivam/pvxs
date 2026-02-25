/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */
#define PVXS_ENABLE_EXPERT_API

#include <atomic>

#include <testMain.h>

#include <epicsUnitTest.h>

#include <epicsEvent.h>
#include <epicsThread.h>

#include <pvxs/unittest.h>
#include <pvxs/log.h>
#include <pvxs/client.h>
#include <pvxs/server.h>
#include <pvxs/sharedpv.h>
#include <pvxs/source.h>
#include <pvxs/nt.h>

namespace {
using namespace pvxs;

struct Tester {
    Value initial;
    server::SharedPV mbox;
    server::Server serv;
    client::Context cli;

    Tester()
        :initial(nt::NTScalar{TypeCode::Int32}.create())
        ,mbox(server::SharedPV::buildMailbox())
        ,serv(server::Config::isolated()
              .build()
              .addPV("mailbox", mbox))
        ,cli(serv.clientConfig().build())
    {
        testShow()<<"Server:\n"<<serv.config()
                  <<"Client:\n"<<cli.config();

        initial["value"] = 42;
    }

    ~Tester()
    {
        if(cli.use_count()>1u)
            testAbort("Tester Context leak: %u", unsigned(cli.use_count()));
    }

    void testDefaultPermissions()
    {
        testShow()<<__func__;

        mbox.open(initial);
        serv.start();

        epicsEvent connEvt;
        epicsEvent aclEvt;
        std::atomic<uint8_t> gotPerm{0xff};

        auto conn = cli.connect("mailbox")
                .onConnect([&connEvt]() {
                    connEvt.signal();
                })
                .onPermissions([&gotPerm, &aclEvt](uint8_t perm) {
                    gotPerm = perm;
                    aclEvt.signal();
                })
                .exec();

        testTrue(connEvt.wait(5.0))<<" Wait for connect";
        testTrue(aclEvt.wait(2.0))<<" Wait for initial ACL callback";
        testTrue(conn->connected());
        // default permissions = 0x07 (full access)
        testEq(conn->permissions(), 0x07u)<<" Default permissions via polling";
        testEq(gotPerm.load(), 0x07u)<<" Default permissions via callback";

        serv.stop();
    }

    void testDynamicACLChange()
    {
        testShow()<<__func__;

        mbox.open(initial);
        serv.start();

        epicsEvent connEvt;
        epicsEvent aclEvt;
        std::atomic<uint8_t> gotPerm{0xff};

        auto conn = cli.connect("mailbox")
                .onConnect([&connEvt]() {
                    connEvt.signal();
                })
                .onPermissions([&gotPerm, &aclEvt](uint8_t perm) {
                    gotPerm = perm;
                    aclEvt.signal();
                })
                .exec();

        testTrue(connEvt.wait(5.0))<<" Wait for connect";
        testTrue(aclEvt.wait(2.0))<<" Wait for initial ACL callback";
        testTrue(conn->connected());

        // Now change permissions from the server side
        mbox.setPermissions(0x01); // PUT only

        // wait for the dynamic ACL update
        while(gotPerm.load() != 0x01) {
            if(!aclEvt.wait(5.0))
                break;
        }
        testEq(gotPerm.load(), 0x01u)<<" Updated permissions via callback";
        testEq(conn->permissions(), 0x01u)<<" Updated permissions via polling";

        serv.stop();
    }

    void testOperationPermissions()
    {
        testShow()<<__func__;

        mbox.open(initial);
        serv.start();

        auto op = cli.get("mailbox").exec();
        auto result = op->wait(5.0);

        // default permissions should be 0x07
        testEq(op->permissions(), 0x07u)<<" Operation default permissions";

        serv.stop();
    }

    void testSubscriptionPermissions()
    {
        testShow()<<__func__;

        mbox.open(initial);
        serv.start();

        epicsEvent update;
        auto sub = cli.monitor("mailbox")
                .maskConnected(true)
                .maskDisconnected(true)
                .event([&update](client::Subscription&) {
                    update.signal();
                })
                .exec();

        // wait for initial update
        testTrue(update.wait(5.0))<<" Wait for subscription update";
        auto val = sub->pop();
        testTrue(!!val)<<" Got subscription value";

        // default permissions should be 0x07
        testEq(sub->permissions(), 0x07u)<<" Subscription default permissions";

        // Change permissions and verify subscription sees them
        mbox.setPermissions(0x05); // PUT + RPC
        // Give the ACL message time to arrive
        epicsThreadSleep(0.5);
        testEq(sub->permissions(), 0x05u)<<" Subscription updated permissions";

        serv.stop();
    }

    void testSubscriptionAclChanged()
    {
        testShow()<<__func__;

        mbox.open(initial);
        serv.start();

        epicsEvent update;
        auto sub = cli.monitor("mailbox")
                .maskConnected(true)
                .maskDisconnected(true)
                .maskACLChange(false) // enable AclChanged notifications
                .event([&update](client::Subscription&) {
                    update.signal();
                })
                .exec();

        // wait for initial update
        testTrue(update.wait(5.0))<<" Wait for subscription update";
        auto val = sub->pop();
        testTrue(!!val)<<" Got subscription value";
        while(sub->pop()) {} // drain remaining entries

        // Change permissions from server side
        mbox.setPermissions(0x01); // PUT only

        // Wait for AclChanged - loop following testmon.cpp pattern,
        // so a spurious event wake doesn't cause us to miss the exception.
        uint8_t caughtPerm = 0xff;
        try {
            while(true) {
                if(sub->pop()) continue; // skip any stray values
                if(!update.wait(5.0))
                    testAbort("Timeout waiting for AclChanged notification");
            }
        } catch(client::AclChanged& e) {
            caughtPerm = e.permissions;
        } catch(std::exception& e) {
            testFail("Unexpected exception: %s", e.what());
        }
        testEq(caughtPerm, 0x01u)<<" AclChanged carries correct permissions";
        testEq(sub->permissions(), 0x01u)<<" Permissions updated via polling";

        serv.stop();
    }

    void testSubscriptionAclChangedMasked()
    {
        testShow()<<__func__;

        mbox.open(initial);
        serv.start();

        epicsEvent update;
        epicsEvent aclEvt;
        std::atomic<uint8_t> gotPerm{0xff};

        // Subscribe with default maskACLChange (true = suppress AclChanged)
        auto sub = cli.monitor("mailbox")
                .maskConnected(true)
                .maskDisconnected(true)
                .event([&update](client::Subscription&) {
                    update.signal();
                })
                .exec();

        // Use a Connect on the same channel to know when the ACL update
        // has been processed by the event loop, without relying on a sleep.
        auto conn = cli.connect("mailbox")
                .onPermissions([&gotPerm, &aclEvt](uint8_t perm) {
                    gotPerm = perm;
                    aclEvt.signal();
                })
                .exec();

        // wait for initial subscription value
        testTrue(update.wait(5.0))<<" Wait for subscription update";
        auto val = sub->pop();
        testTrue(!!val)<<" Got subscription value";
        while(sub->pop()) {} // drain

        // Change permissions from server side
        mbox.setPermissions(0x01);

        // Wait for the ACL update to arrive via the connect callback.
        // handle_ACL_CHANGE() sets chan->permissions then calls connector
        // callbacks before operation callbacks, so once onPermissions fires
        // the event loop is done with the ACL update for this channel.
        while(gotPerm.load() != 0x01) {
            if(!aclEvt.wait(5.0))
                break;
        }
        testEq(gotPerm.load(), 0x01u)<<" ACL change received on channel";

        // The subscription queue must be empty: AclChanged is suppressed by default
        testFalse(sub->pop())<<" No AclChanged in queue when masked (default)";

        serv.stop();
    }

    void testACLResetOnDisconnect()
    {
        testShow()<<__func__;

        mbox.open(initial);
        serv.start();

        epicsEvent connEvt;
        epicsEvent disEvt;
        epicsEvent aclEvt;
        std::atomic<uint8_t> gotPerm{0xff};

        auto conn = cli.connect("mailbox")
                .onConnect([&connEvt]() {
                    connEvt.signal();
                })
                .onDisconnect([&disEvt]() {
                    disEvt.signal();
                })
                .onPermissions([&gotPerm, &aclEvt](uint8_t perm) {
                    gotPerm = perm;
                    aclEvt.signal();
                })
                .exec();

        testTrue(connEvt.wait(5.0))<<" Wait for connect";
        // wait for initial ACL
        aclEvt.wait(2.0);

        // Change permissions
        mbox.setPermissions(0x01);
        while(gotPerm.load() != 0x01) {
            if(!aclEvt.wait(5.0))
                break;
        }
        testEq(gotPerm.load(), 0x01u);

        // Stop server -> disconnect -> permissions should reset
        serv.stop();
        testTrue(disEvt.wait(5.0))<<" Wait for disconnect";

        // After disconnect, polling should return default 0x07
        testEq(conn->permissions(), 0x07u)<<" Permissions reset after disconnect";

        // Restart server -> reconnect -> should get fresh default permissions (0x07)
        serv.start();
        testTrue(connEvt.wait(5.0))<<" Wait for reconnect";
        while(gotPerm.load() != 0x07) {
            if(!aclEvt.wait(5.0))
                break;
        }
        testEq(gotPerm.load(), 0x07u)<<" Permissions restored after reconnect";

        serv.stop();
    }
};

// Custom Source that sets permissions in onCreate
struct RestrictedSource : public server::Source
{
    uint8_t perm;
    std::shared_ptr<server::ChannelControl> savedChan;

    RestrictedSource(uint8_t p) :perm(p) {}

    virtual void onSearch(Search &op) override final
    {
        for(auto& name : op) {
            if(std::string(name.name()) == "restricted")
                name.claim();
        }
    }

    virtual void onCreate(std::unique_ptr<server::ChannelControl> &&op) override final
    {
        auto chan = std::shared_ptr<server::ChannelControl>(std::move(op));
        chan->setPermissions(perm);

        chan->onOp([](std::unique_ptr<server::ConnectOp>&& op) {
            auto prototype = nt::NTScalar{TypeCode::Int32}.create();
            op->connect(prototype);
            op->onGet([prototype](std::unique_ptr<server::ExecOp>&& op) {
                auto val = prototype.clone();
                val["value"] = 100;
                op->reply(val);
            });
        });

        savedChan = chan;
    }
};

void testCustomInitialPermissions()
{
    testShow()<<__func__;

    auto src = std::make_shared<RestrictedSource>(0x01); // PUT only

    auto serv = server::Config::isolated()
            .build()
            .addSource("test", src)
            .start();

    auto cli = serv.clientConfig().build();

    epicsEvent connEvt;
    epicsEvent aclEvt;
    std::atomic<uint8_t> gotPerm{0xff};

    auto conn = cli.connect("restricted")
            .onConnect([&connEvt]() {
                connEvt.signal();
            })
            .onPermissions([&gotPerm, &aclEvt](uint8_t perm) {
                gotPerm = perm;
                aclEvt.signal();
            })
            .exec();

    testTrue(connEvt.wait(5.0))<<" Wait for connect";
    // The initial ACL should have been received before channel creation
    // Small delay to allow ACL message processing
    aclEvt.wait(2.0);
    testEq(conn->permissions(), 0x01u)<<" Custom initial permissions via polling";
    testEq(gotPerm.load(), 0x01u)<<" Custom initial permissions via callback";
}

} // namespace

MAIN(testacl)
{
    testPlan(32);
    testSetup();
    logger_config_env();
    Tester().testDefaultPermissions();
    Tester().testDynamicACLChange();
    Tester().testOperationPermissions();
    Tester().testSubscriptionPermissions();
    Tester().testSubscriptionAclChanged();
    Tester().testSubscriptionAclChangedMasked();
    Tester().testACLResetOnDisconnect();
    testCustomInitialPermissions();
    cleanup_for_valgrind();
    return testDone();
}
