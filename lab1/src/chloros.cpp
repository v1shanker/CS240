#include "chloros.h"
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <queue>
#include <vector>
#include "common.h"

extern "C" {

// Assembly code to switch context from `old_context` to `new_context`.
void ContextSwitch(chloros::Context* old_context,
                   chloros::Context* new_context) __asm__("context_switch");

// Assembly entry point for a thread that is spawn. It will fetch arguments on
// the stack and put them in the right registers. Then it will call
// `ThreadEntry` for further setup.
void StartThread(void* arg) __asm__("start_thread");
}

namespace chloros {

namespace {

// Default stack size is 2 MB.
constexpr int const kStackSize{1 << 21};

// Queue of threads that are not running.
std::vector<std::unique_ptr<Thread>> thread_queue{};

// Mutex protecting the queue, which will potentially be accessed by multiple
// kernel threads.
std::mutex queue_lock{};

// Current running thread. It is local to the kernel thread.
thread_local std::unique_ptr<Thread> current_thread{nullptr};

// Thread ID of initial thread. In extra credit phase, we want to switch back to
// the initial thread corresponding to the kernel thread. Otherwise there may be
// errors during thread cleanup. In other words, if the initial thread is
// spawned on this kernel thread, never switch it to another kernel thread!
// Think about how to achieve this in `Yield` and using this thread-local
// `initial_thread_id`.
thread_local uint64_t initial_thread_id;

}  // anonymous namespace

std::atomic<uint64_t> Thread::next_id;

Thread::Thread(bool create_stack)
    : id{0}, state{State::kWaiting}, context{}, stack{nullptr}, stack_bottom{nullptr} {
  // The first thread id should be 0.
  // Since Thread::next_id is std::atomic, the ++ operator is atomic.
  id = Thread::next_id++;

  if (create_stack) {
    // malloc 2MB for the stack.
    stack_bottom = (uint8_t *) malloc(kStackSize);
    // 16 byte align the address.
    // Subtract 16 bytes to make sure we stay in the malloc'ed memory after
    // aligning the address.
    stack = (uint8_t *) (((uintptr_t) (stack_bottom + kStackSize - 0x10)) & ~0x0F);
  }

  // These two initial values are provided for you.
  context.mxcsr = 0x1F80;
  context.x87 = 0x037F;

  // Set kernel thread_id.
  kernel_tid = std::this_thread::get_id();
}

Thread::~Thread() {
  // stack_bottom is only non-null if a stack was allocated.
  if (stack_bottom) {
    free(stack_bottom);
  }
}

void Thread::PrintDebug() {
  fprintf(stderr, "Thread %" PRId64 ": ", id);
  switch (state) {
    case State::kWaiting:
      fprintf(stderr, "waiting");
      break;
    case State::kReady:
      fprintf(stderr, "ready");
      break;
    case State::kRunning:
      fprintf(stderr, "running");
      break;
    case State::kZombie:
      fprintf(stderr, "zombie");
      break;
    default:
      break;
  }
  fprintf(stderr, "\n\tStack: %p\n", stack);
  fprintf(stderr, "\tRSP: 0x%" PRIx64 "\n", context.rsp);
  fprintf(stderr, "\tR15: 0x%" PRIx64 "\n", context.r15);
  fprintf(stderr, "\tR14: 0x%" PRIx64 "\n", context.r14);
  fprintf(stderr, "\tR13: 0x%" PRIx64 "\n", context.r13);
  fprintf(stderr, "\tR12: 0x%" PRIx64 "\n", context.r13);
  fprintf(stderr, "\tRBX: 0x%" PRIx64 "\n", context.rbx);
  fprintf(stderr, "\tRBP: 0x%" PRIx64 "\n", context.rbp);
  fprintf(stderr, "\tMXCSR: 0x%x\n", context.mxcsr);
  fprintf(stderr, "\tx87: 0x%x\n", context.x87);
}

void Initialize() {
  auto new_thread = std::make_unique<Thread>(false);
  new_thread->state = Thread::State::kWaiting;
  initial_thread_id = new_thread->id;
  current_thread = std::move(new_thread);
}

void Spawn(Function fn, void* arg) {
  auto new_thread = std::make_unique<Thread>(true);

  // Setup the stack
  // addr is stored on the stack and ret pops it off when context switching.
  // The function we want to call is start_thread, which expects fn and arg to
  // be on the stack so the new_thread stack should loook like this:
  //
  //   |----------------|
  //   |       &arg     |
  //   |----------------|
  //   |       &fn      |
  //   |----------------|
  //   | &start_thread  |
  //   |----------------| <-- %rsp

  std::memcpy((void *) (new_thread->stack), &arg, sizeof(arg));
  new_thread->stack -= 0x8;
  std::memcpy((void *) (new_thread->stack), &fn, sizeof(fn));
  new_thread->stack -= 0x8;
  uint64_t start_thread_ptr = (uint64_t) &StartThread;
  std::memcpy((void *) (new_thread->stack), &start_thread_ptr, sizeof(start_thread_ptr));

  // Set the %rsp register now that we have changed the stack.
  (new_thread->context).rsp = (uint64_t) (new_thread->stack);

  // Mark the thread as ready
  new_thread->state = Thread::State::kReady;

  // Insert the thread into the front of the queue and yield to it
  // to make sure it runs immediately.
  queue_lock.lock();
  thread_queue.insert(thread_queue.begin(), std::move(new_thread));
  queue_lock.unlock();
  Yield(true);
}

bool Yield(bool only_ready) {
  unsigned int new_thread_id;
  bool new_thr_found = false;

  queue_lock.lock();

  std::thread::id curr_kernel_tid = std::this_thread::get_id();
  // Find a candidate thread
  for(unsigned int i = 0; i < thread_queue.size(); i++) {
    Thread::State i_state = thread_queue[i]->state;
    // If we are not on the initial kernel thread and the thread is the
    // initial thread, skip the thread.
    if ((curr_kernel_tid != thread_queue[i]->kernel_tid) && (thread_queue[i]->id == initial_thread_id)) {
      continue;
    }
    if (only_ready && i_state == Thread::State::kReady) {
      new_thread_id = i;
      new_thr_found = true;
      break;
    } else if (!only_ready && (i_state == Thread::State::kReady || i_state == Thread::State::kWaiting)) {
      new_thread_id = i;
      new_thr_found = true;
      break;
    }
  }

  // If we do not find a new thread...
  if (!new_thr_found) {
    queue_lock.unlock();
    return false;
  }

  // Waiting threads shouldn't be erroneously moved to ready since Wait()
  // calls Yield
  if (current_thread->state == Thread::State::kRunning) {
    current_thread->state = Thread::State::kReady;
  }
  thread_queue.push_back(std::move(current_thread));

  current_thread = std::move(thread_queue[new_thread_id]);
  thread_queue.erase(thread_queue.begin() + new_thread_id);

  Context* old_ctx = &(thread_queue[thread_queue.size() - 1]->context);
  Context* new_ctx = &(current_thread->context);
  ContextSwitch(old_ctx, new_ctx);

  current_thread->state = Thread::State::kRunning;

  // Threads are marked as Zombies in ThreadEntry after the function finishes.
  // ThreadEntry then Yields, therefore, in Yield is the earliest we can
  // GarbageCollect.
  GarbageCollect();
  queue_lock.unlock();
  return true;
}

void Wait() {
  current_thread->state = Thread::State::kWaiting;
  while (Yield(true)) {
    current_thread->state = Thread::State::kWaiting;
  }
}

// This function must be called with the queue_lock held
void GarbageCollect() {
  // Use two loops because I don't know how to concurrently modify a vector
  // while looping over it in C++ :(

  // Find all the zombie thread ids...
  std::set<int> zombie_thr_ids = {};
  for(unsigned int i = 0; i < thread_queue.size(); i++) {
    if (thread_queue[i]->state == Thread::State::kZombie) {
      zombie_thr_ids.insert(i);
    }
  }

  // ... then delete them.
  for(std::set<int>::iterator it = zombie_thr_ids.begin(); it != zombie_thr_ids.end(); it++) {
    thread_queue.erase(thread_queue.begin() + *it);
  }
}

std::pair<int, int> GetThreadCount() {
  // Please don't modify this function.
  int ready = 0;
  int zombie = 0;
  std::lock_guard<std::mutex> lock{queue_lock};
  for (auto&& i : thread_queue) {
    if (i->state == Thread::State::kZombie) {
      ++zombie;
    } else {
      ++ready;
    }
  }
  return {ready, zombie};
}

void ThreadEntry(Function fn, void* arg) {
  queue_lock.unlock();
  fn(arg);
  current_thread->state = Thread::State::kZombie;
  LOG_DEBUG("Thread %" PRId64 " exiting.", current_thread->id);
  // A thread that is spawn will always die yielding control to other threads.
  chloros::Yield();
  // Unreachable here. Why?
  ASSERT(false);
}

}  // namespace chloros
