#include "config.h"
#include <unistd.h>
#include <cassert>

#include "dispatcher.hh"
#include "atomic.hh"
#include "locks.hh"
#include "priority.hh"

Dispatcher dispatcher;
static Atomic<int> callbacks;

class Thing;

class TestCallback : public DispatcherCallback {
public:
    TestCallback(Thing *t) : thing(t) {
    }

    bool callback(Dispatcher &d, TaskId t);

    std::string description() { return std::string("Test"); }

private:
    Thing *thing;
};

class Thing {
public:
    void start(double sleeptime=0) {
        dispatcher.schedule(shared_ptr<TestCallback>(new TestCallback(this)),
                            NULL, Priority::BgFetcherPriority, sleeptime);
        dispatcher.schedule(shared_ptr<TestCallback>(new TestCallback(this)),
                            NULL, Priority::FlusherPriority, sleeptime);
        dispatcher.schedule(shared_ptr<TestCallback>(new TestCallback(this)),
                            NULL, Priority::VBucketDeletionPriority, 0, false);
    }

    bool doSomething(Dispatcher &d, TaskId &t) {
        (void)d; (void)t;
        ++callbacks;
        return false;
    }
};

bool TestCallback::callback(Dispatcher &d, TaskId t) {
    return thing->doSomething(d, t);
}

extern "C" {

static const char* test_get_logger_name(void) {
    return "dispatcher_test";
}

static void test_get_logger_log(EXTENSION_LOG_LEVEL severity,
                                const void* client_cookie,
                                const char *fmt, ...) {
    (void)severity;
    (void)client_cookie;
    (void)fmt;
    // ignore
}
}

EXTENSION_LOGGER_DESCRIPTOR* getLogger() {
    static EXTENSION_LOGGER_DESCRIPTOR logger;
    logger.get_name = test_get_logger_name;
    logger.log = test_get_logger_log;
    return &logger;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    int expected_num_callbacks=3;
    Thing t;

    alarm(5);
    dispatcher.start();
    t.start();
    // Wait for some callbacks
    while (callbacks < expected_num_callbacks) {
        usleep(1);
    }
    if (callbacks != expected_num_callbacks) {
        std::cerr << "Expected " << expected_num_callbacks << " callbacks, but got "
                  << callbacks << std::endl;
        return 1;
    }

    callbacks=0;
    expected_num_callbacks=1;
    t.start(3);
    dispatcher.stop();
    if (callbacks != expected_num_callbacks) {
        std::cerr << "Expected " << expected_num_callbacks << " callbacks, but got "
                  << callbacks << std::endl;
        return 1;
    }
    return 0;
}
