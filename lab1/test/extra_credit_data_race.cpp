#include <chloros.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>


constexpr int const kKernelThreads = 5;

class MockFile {
  public:
    std::string name;
  public:
    MockFile(std::string n) {name = n;}
    void open() {}
    void close() {}
};


MockFile *mock_file = nullptr;
std::mutex file_lock;

/*
 * If no singleton instance is set, set the instance, otherwise use it.
 * The code is slightly edited from the paper in that the shared object
 * is not using the C++ FILE API, but rather a mock.
 */
void GreenThreadWorker(void*) {
  if (mock_file == nullptr) {
    file_lock.lock();
    if (mock_file == nullptr) {
      mock_file = new MockFile("foo.txt");
    }
    file_lock.unlock();
  }
}

void KernelThreadWorker(int n) {
  chloros::Initialize();
  chloros::Spawn(GreenThreadWorker, nullptr);
  chloros::Wait();
  printf("Finished thread %d.\n", n);
}

int main() {
  std::vector<std::thread> threads{};
  for (int i = 0; i < kKernelThreads; ++i) {
    threads.emplace_back(KernelThreadWorker, i);
  }
  for (int i = 0; i < kKernelThreads; ++i) {
    threads[i].join();
  }
  return 0;
}
