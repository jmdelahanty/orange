#include "bounded_object_pool.h"

#include <cassert>
#include <string>

namespace {

struct TestObject {
    int value = 0;
    int reset_count = 0;
    std::string text;
};

struct TestObjectReset {
    void operator()(TestObject& object) const
    {
        object.value = 0;
        object.text.clear();
        ++object.reset_count;
    }
};

void test_borrow_return_and_reset()
{
    BoundedObjectPool<TestObject, TestObjectReset> pool("test", 2);

    TestObject* first = pool.Borrow();
    TestObject* second = pool.Borrow();
    assert(first != nullptr);
    assert(second != nullptr);
    assert(first != second);

    first->value = 42;
    first->text = "kept-capacity";
    const size_t first_capacity = first->text.capacity();
    assert(pool.Borrow() == nullptr);

    auto stats = pool.GetStats();
    assert(stats.capacity == 2);
    assert(stats.available == 0);
    assert(stats.active == 2);
    assert(stats.high_water == 2);
    assert(stats.borrow_misses == 1);

    assert(pool.Return(first));
    stats = pool.GetStats();
    assert(stats.available == 1);
    assert(stats.active == 1);
    assert(stats.double_returns == 0);
    assert(stats.invalid_returns == 0);

    TestObject* reused = pool.Borrow();
    assert(reused == first);
    assert(reused->value == 0);
    assert(reused->text.empty());
    assert(reused->text.capacity() >= first_capacity);
    assert(reused->reset_count == 1);

    assert(pool.Return(reused));
    assert(pool.Return(second));
    stats = pool.GetStats();
    assert(stats.available == 2);
    assert(stats.active == 0);
}

void test_invalid_and_double_return()
{
    BoundedObjectPool<TestObject, TestObjectReset> pool("test", 1);
    TestObject outside;

    assert(!pool.Return(&outside));
    auto stats = pool.GetStats();
    assert(stats.invalid_returns == 1);

    TestObject* object = pool.Borrow();
    assert(object != nullptr);
    assert(pool.Return(object));
    assert(!pool.Return(object));

    stats = pool.GetStats();
    assert(stats.double_returns == 1);
    assert(stats.available == 1);
    assert(stats.active == 0);
}

void test_empty_pool_exhaustion()
{
    BoundedObjectPool<TestObject, TestObjectReset> pool("empty", 0);
    assert(pool.Borrow() == nullptr);
    const auto stats = pool.GetStats();
    assert(stats.capacity == 0);
    assert(stats.available == 0);
    assert(stats.active == 0);
    assert(stats.borrow_misses == 1);
}

}  // namespace

int main()
{
    test_borrow_return_and_reset();
    test_invalid_and_double_return();
    test_empty_pool_exhaustion();
    return 0;
}
