// COM 智能指针，以 RAII 方式管理引用计数。
#pragma once

#include <unknwn.h>

template <class T>
class Com {
public:
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    Com& operator=(Com&& o) noexcept {
        if (this != &o) {
            Reset();
            p_ = o.p_;
            o.p_ = nullptr;
        }
        return *this;
    }
    ~Com() { Reset(); }

    void Reset() {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }
    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

    // 返回用于接收输出参数的二级指针，调用前先释放旧值。
    T** Put() {
        Reset();
        return &p_;
    }
    void** PutVoid() {
        Reset();
        return reinterpret_cast<void**>(&p_);
    }
    T* Detach() {
        T* p = p_;
        p_ = nullptr;
        return p;
    }
    void Attach(T* p) {
        Reset();
        p_ = p;
    }

private:
    T* p_ = nullptr;
};
