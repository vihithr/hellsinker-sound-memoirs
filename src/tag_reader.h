// Library metadata stays off the window thread. The UI owns all catalog order;
// the reader only handles immutable paths and returns tag values.
#pragma once
#include "catalog.h"
#include <objbase.h>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

class TagReader {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::wstring> pending;
    std::map<std::wstring,catalog::Tags> values;
    std::thread worker;
    bool closing=false, updated=false;
    void run() {
        HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        for(;;) {
            std::unique_lock<std::mutex> lock(mutex);
            ready.wait(lock,[&]{return closing || !pending.empty();});
            if(closing) break;
            auto path=std::move(pending.front());pending.pop_front();
            lock.unlock();
            auto tags=catalog::tags(path);
            lock.lock();
            values[path]=std::move(tags);updated=true;
        }
        if(SUCCEEDED(com)) CoUninitialize();
    }
public:
    void enqueue(const std::wstring& path) {
        std::lock_guard<std::mutex> lock(mutex);
        if(!values.emplace(path,catalog::Tags{}).second) return;
        pending.push_back(path);
        if(!worker.joinable()) worker=std::thread([this]{run();});
        ready.notify_one();
    }
    catalog::Tags get(const std::wstring& p) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it=values.find(p);return it==values.end()?catalog::Tags{}:it->second;
    }
    bool changed() {
        std::lock_guard<std::mutex> lock(mutex);
        bool result=updated;updated=false;return result;
    }
    ~TagReader() {
        {std::lock_guard<std::mutex> lock(mutex);closing=true;}
        ready.notify_one();if(worker.joinable())worker.join();
    }
};

