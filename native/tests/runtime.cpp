#include "runtime.hpp"
#include <cstdlib>
#include <iostream>
using namespace iostodroid::runtime;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error("CHECK failed: " #x);                                                   \
    } while (0)
struct Tracked : Object {
    int &destroyed;
    Tracked(Class *c, int &d) : Object(c), destroyed(d) {}
    ~Tracked() override {
        destroyed++;
    }
};
int main() {
    Runtime runtime;
    auto base = runtime.registerClass("Base", nullptr, {"value"});
    auto child = runtime.registerClass("Child", base, {"other"});
    auto value = runtime.selector("value");
    CHECK(value == runtime.selector("value"));
    runtime.addMethod(base, value, [](Object *o, const Arguments &) { return intptr_t(o->ivars[0]); });
    auto object = runtime.allocate(child);
    object->ivars[0] = 42;
    CHECK(runtime.send(object, value) == 42);
    CHECK(object->ivars.size() == 2);
    runtime.addCategory(base, {{value, [](Object *, const Arguments &) { return 99; }}});
    CHECK(runtime.send(object, value) == 99);
    runtime.addProperty(base, {"value", "Ti"});
    runtime.registerProtocol({"Valued", {value}});
    runtime.adoptProtocol(child, "Valued");
    CHECK(child->metaclass->super == base->metaclass);
    CHECK(runtime.findClass("Child") == child);
    CHECK(runtime.send(nullptr, value) == 0);
    bool threw = false;
    try {
        runtime.send(object, runtime.selector("missing"));
    } catch (const std::runtime_error &) {
        threw = true;
    }
    CHECK(threw);
    int destroyed = 0;
    {
        AutoreleasePool outer;
        AutoreleasePool::add(new Tracked(base, destroyed));
        {
            AutoreleasePool inner;
            AutoreleasePool::add(new Tracked(base, destroyed));
        }
        CHECK(destroyed == 1);
    }
    CHECK(destroyed == 2);
    auto str = new String(base, "hello UTF-8 \xc4\x8d");
    auto array = new Array(base, {str});
    auto dict = new Dictionary(base);
    dict->put("key", str);
    release(str);
    CHECK(dynamic_cast<String *>(array->at(0))->utf8 == "hello UTF-8 \xc4\x8d");
    CHECK(dict->get("key") == array->at(0));
    release(array);
    release(dict);
    auto data = new Data(base, {0, 1, 255});
    CHECK(data->bytes.size() == 3);
    release(data);
    NotificationCenter center;
    int calls = 0;
    auto token = center.observe("event", [&](const std::string &s) {
        CHECK(s == "payload");
        calls++;
    });
    center.post("event", "payload");
    center.remove(token);
    center.post("event", "payload");
    CHECK(calls == 1);
    std::atomic<int> count{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; i++)
        threads.emplace_back([&, i] {
            thread_local int local = 0;
            local = i;
            for (int n = 0; n < 1000; n++) {
                retain(object);
                CHECK(runtime.send(object, value) == 99);
                release(object);
                count++;
            }
            CHECK(local == i);
        });
    for (auto &t : threads)
        t.join();
    CHECK(count == 8000);
    CHECK(object->references == 1);
    release(object);
    std::mutex mutex;
    std::condition_variable condition;
    bool ready = false;
    std::thread worker([&] {
        std::unique_lock<std::mutex> lock(mutex);
        ready = true;
        condition.notify_one();
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return ready; });
    }
    worker.join();
    auto memory = std::malloc(100);
    CHECK(memory);
    std::free(memory);
    auto tmp =
        std::filesystem::temp_directory_path() /
        ("iostodroid-runtime-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Sandbox fs(tmp);
    fs.write("Documents", "nested/file.txt", "data");
    CHECK(fs.read("Documents", "nested/file.txt") == "data");
    for (auto path : {"../escape", "/absolute"}) {
        bool blocked = false;
        try {
            fs.resolve("Documents", path);
        } catch (const std::runtime_error &) {
            blocked = true;
        }
        CHECK(blocked);
    }
    std::filesystem::create_symlink(tmp / "Documents/nested/file.txt", tmp / "Documents/link");
    bool blocked = false;
    try {
        fs.read("Documents", "link");
    } catch (const std::runtime_error &) {
        blocked = true;
    }
    CHECK(blocked);
    std::filesystem::remove_all(tmp);
    std::cout << "portable runtime: "
                 "dispatch/cache/category/metaclass/protocol/ivars/refcounts/autorelease/data/notifications/"
                 "filesystem/threads/TLS/RTTI/exceptions passed\n";
}
