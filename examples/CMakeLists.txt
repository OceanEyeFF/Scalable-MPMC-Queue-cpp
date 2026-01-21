function(lscq_add_example target)
  add_executable(${target} ${ARGN})

  target_link_libraries(${target}
    PRIVATE
      lscq::lscq
      lscq::lscq_impl
  )

  set_target_properties(${target} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
  )

  if(LSCQ_ENABLE_SANITIZERS AND DEFINED LSCQ_ASAN_DLL AND EXISTS "${LSCQ_ASAN_DLL}")
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LSCQ_ASAN_DLL}" "$<TARGET_FILE_DIR:${target}>"
    )
  endif()
endfunction()

lscq_add_example(simple_queue
  simple_queue.cpp
)

lscq_add_example(benchmark_custom
  benchmark_custom.cpp
)

lscq_add_example(simple_usage
  simple_usage.cpp
)

lscq_add_example(task_queue
  task_queue.cpp
)

lscq_add_example(producer_consumer
  producer_consumer.cpp
)

lscq_add_example(benchmark_demo
  benchmark_demo.cpp
)

add_custom_target(examples DEPENDS
  simple_queue
  benchmark_custom
  simple_usage
  task_queue
  producer_consumer
  benchmark_demo
)
