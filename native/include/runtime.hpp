#pragma once
// Experimental portable runtime. NOT a binary-compatible Apple ObjC runtime.
// No converted image may bind to this module until metadata/ABI adapters exist.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
namespace radek::runtime {
using Selector = uint64_t;
struct Class;
struct Object {
    Class *isa;
    std::atomic<uint32_t> references{1};
    std::vector<uintptr_t> ivars;
    explicit Object(Class *c, size_t n = 0) : isa(c), ivars(n) {}
    virtual ~Object() = default;
    Object(const Object &) = delete;
};
using Arguments = std::vector<intptr_t>;
using IMP = std::function<intptr_t(Object *, const Arguments &)>;
struct Property {
    std::string name, attributes;
};
struct Protocol {
    std::string name;
    std::set<Selector> required;
};
struct Class {
    std::string name;
    Class *super = nullptr, *metaclass = nullptr;
    std::map<Selector, IMP> methods;
    std::vector<std::string> ivarNames;
    std::vector<Property> properties;
    std::set<std::string> protocols;
    size_t instanceSlots = 0;
};
class Runtime {
    std::mutex mutex_;
    std::map<std::string, Selector> selectors_;
    std::map<std::string, std::unique_ptr<Class>> classes_;
    std::map<std::string, Protocol> protocols_;
    std::map<std::pair<Class *, Selector>, IMP> cache_;
    IMP lookupLocked(Class *c, Selector s) {
        auto key = std::make_pair(c, s);
        auto hit = cache_.find(key);
        if (hit != cache_.end())
            return hit->second;
        for (auto p = c; p; p = p->super) {
            auto method = p->methods.find(s);
            if (method != p->methods.end()) {
                cache_[key] = method->second;
                return method->second;
            }
        }
        throw std::runtime_error("unrecognized selector: " + std::to_string(s));
    }

  public:
    Selector selector(const std::string &name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto [i, inserted] = selectors_.emplace(name, selectors_.size() + 1);
        (void)inserted;
        return i->second;
    }
    Class *registerClass(const std::string &name, Class *super = nullptr,
                         std::vector<std::string> ivars = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (name.empty() || classes_.count(name) || classes_.count(name + "$meta"))
            throw std::runtime_error("duplicate/invalid class");
        auto c = std::make_unique<Class>();
        auto m = std::make_unique<Class>();
        c->name = name;
        c->super = super;
        c->ivarNames = std::move(ivars);
        c->instanceSlots = (super ? super->instanceSlots : 0) + c->ivarNames.size();
        m->name = name + "$meta";
        m->super = super ? super->metaclass : nullptr;
        m->metaclass = super ? super->metaclass->metaclass : m.get();
        c->metaclass = m.get();
        auto result = c.get();
        classes_.emplace(name, std::move(c));
        classes_.emplace(m->name, std::move(m));
        return result;
    }
    Class *findClass(const std::string &name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto i = classes_.find(name);
        return i == classes_.end() ? nullptr : i->second.get();
    }
    void addMethod(Class *c, Selector s, IMP imp) {
        if (!c || !imp)
            throw std::invalid_argument("invalid method");
        std::lock_guard<std::mutex> lock(mutex_);
        c->methods[s] = std::move(imp);
        cache_.clear();
    }
    void addCategory(Class *c, const std::map<Selector, IMP> &methods) {
        if (!c)
            throw std::invalid_argument("invalid category");
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto &[s, imp] : methods) {
            if (!imp)
                throw std::invalid_argument("invalid category IMP");
            c->methods[s] = imp;
        }
        cache_.clear();
    }
    void addProperty(Class *c, Property p) {
        std::lock_guard<std::mutex> lock(mutex_);
        c->properties.push_back(std::move(p));
    }
    void registerProtocol(Protocol p) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!protocols_.emplace(p.name, std::move(p)).second)
            throw std::runtime_error("duplicate protocol");
    }
    void adoptProtocol(Class *c, const std::string &name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto p = protocols_.find(name);
        if (p == protocols_.end())
            throw std::runtime_error("unknown protocol");
        for (auto s : p->second.required)
            lookupLocked(c, s);
        c->protocols.insert(name);
    }
    Object *allocate(Class *c) {
        if (!c)
            throw std::invalid_argument("unknown class");
        return new Object(c, c->instanceSlots);
    }
    intptr_t send(Object *object, Selector selector, const Arguments &args = {}) {
        if (!object)
            return 0;
        IMP imp;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            imp = lookupLocked(object->isa, selector);
        }
        return imp(object, args);
    }
};
inline Object *retain(Object *o) {
    if (o)
        o->references.fetch_add(1, std::memory_order_relaxed);
    return o;
}
inline void release(Object *o) {
    if (o && o->references.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete o;
}
class AutoreleasePool {
    inline static thread_local AutoreleasePool *current_ = nullptr;
    AutoreleasePool *previous_;
    std::vector<Object *> objects_;

  public:
    AutoreleasePool() : previous_(current_) {
        current_ = this;
    }
    ~AutoreleasePool() {
        current_ = previous_;
        for (auto i = objects_.rbegin(); i != objects_.rend(); ++i)
            release(*i);
    }
    AutoreleasePool(const AutoreleasePool &) = delete;
    static Object *add(Object *o) {
        if (!current_)
            throw std::runtime_error("autorelease without pool");
        if (o)
            current_->objects_.push_back(o);
        return o;
    }
};
// Implemented data values, not exported Foundation API symbols.
struct String : Object {
    std::string utf8;
    String(Class *c, std::string s) : Object(c), utf8(std::move(s)) {}
};
struct Data : Object {
    std::vector<uint8_t> bytes;
    Data(Class *c, std::vector<uint8_t> b) : Object(c), bytes(std::move(b)) {}
};
struct Array : Object {
    std::vector<Object *> values;
    Array(Class *c, const std::vector<Object *> &v) : Object(c), values(v) {
        for (auto o : values)
            retain(o);
    }
    ~Array() override {
        for (auto o : values)
            release(o);
    }
    Object *at(size_t index) const {
        return values.at(index);
    }
};
struct Dictionary : Object {
    std::map<std::string, Object *> values;
    explicit Dictionary(Class *c) : Object(c) {}
    ~Dictionary() override {
        for (auto &[k, v] : values) {
            (void)k;
            release(v);
        }
    }
    void put(const std::string &k, Object *v) {
        retain(v);
        auto i = values.find(k);
        if (i != values.end())
            release(i->second);
        values[k] = v;
    }
    Object *get(const std::string &k) const {
        auto i = values.find(k);
        return i == values.end() ? nullptr : i->second;
    }
};
class NotificationCenter {
    std::mutex mutex_;
    uint64_t next_ = 0;
    std::map<uint64_t, std::pair<std::string, std::function<void(const std::string &)>>> observers_;

  public:
    uint64_t observe(std::string name, std::function<void(const std::string &)> fn) {
        std::lock_guard<std::mutex> l(mutex_);
        observers_[++next_] = {std::move(name), std::move(fn)};
        return next_;
    }
    void remove(uint64_t id) {
        std::lock_guard<std::mutex> l(mutex_);
        observers_.erase(id);
    }
    void post(const std::string &name, const std::string &payload) {
        std::vector<std::function<void(const std::string &)>> calls;
        {
            std::lock_guard<std::mutex> l(mutex_);
            for (auto &[id, item] : observers_) {
                (void)id;
                if (item.first == name)
                    calls.push_back(item.second);
            }
        }
        for (auto &fn : calls)
            fn(payload);
    }
};
// Host test fixture storage, not used as a security boundary against hostile
// concurrent processes. Production Android storage is app-private.
class Sandbox {
    std::filesystem::path root_;

  public:
    explicit Sandbox(std::filesystem::path root) : root_(std::filesystem::absolute(root)) {
        std::filesystem::create_directories(root_);
        for (auto dir : {"Documents", "Library", "Caches", "tmp", "bundle"})
            std::filesystem::create_directories(root_ / dir);
    }
    std::filesystem::path resolve(const std::string &area, const std::filesystem::path &relative) const {
        static const std::set<std::string> areas = {"Documents", "Library", "Caches", "tmp", "bundle"};
        if (!areas.count(area) || relative.is_absolute())
            throw std::runtime_error("invalid sandbox path");
        auto result = root_ / area;
        for (auto &component : relative) {
            if (component == ".." || component == ".")
                throw std::runtime_error("sandbox traversal");
            result /= component;
            if (std::filesystem::is_symlink(result))
                throw std::runtime_error("sandbox symlink");
        }
        return result;
    }
    void write(const std::string &area, const std::filesystem::path &relative, const std::string &value) {
        if (area == "bundle")
            throw std::runtime_error("bundle is read-only");
        auto p = resolve(area, relative);
        std::filesystem::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary);
        f << value;
        if (!f)
            throw std::runtime_error("sandbox write failed");
    }
    std::string read(const std::string &area, const std::filesystem::path &relative) {
        std::ifstream f(resolve(area, relative), std::ios::binary);
        if (!f)
            throw std::runtime_error("sandbox read failed");
        return std::string(std::istreambuf_iterator<char>(f), {});
    }
};
} // namespace radek::runtime
