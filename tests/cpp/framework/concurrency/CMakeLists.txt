realm_add_gtest(bounded_queue_test
    SOURCES bounded_queue_test.cpp
    LIBS realm_concurrency
)

realm_add_gtest(bounded_work_pool_test
    SOURCES bounded_work_pool_test.cpp
    LIBS realm_concurrency
)
