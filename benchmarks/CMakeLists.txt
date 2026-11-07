qt_add_executable(ketplus_repository_metadata_benchmark RepositoryMetadataBenchmark.cpp)
target_link_libraries(ketplus_repository_metadata_benchmark PRIVATE ketplus_desktop_support)
ketplus_enable_warnings(ketplus_repository_metadata_benchmark)

qt_add_executable(ketplus_editor_benchmark EditorBenchmark.cpp)
target_link_libraries(ketplus_editor_benchmark PRIVATE
    Qt6::Widgets
    KetPlusCM::editor
)
ketplus_enable_warnings(ketplus_editor_benchmark)

qt_add_executable(ketplus_agent_benchmark AgentWorkspaceBenchmark.cpp)
target_link_libraries(ketplus_agent_benchmark PRIVATE
    Qt6::Quick
    Qt6::Widgets
    ketplus_ai
    KetPlusCM::ui
)
ketplus_enable_warnings(ketplus_agent_benchmark)


qt_add_executable(ketplus_daemon_restore_benchmark DaemonRestoreBenchmark.cpp)
target_link_libraries(ketplus_daemon_restore_benchmark PRIVATE
    Qt6::Core
    ketplus_ai
    ketplus_daemon_core
)
ketplus_enable_warnings(ketplus_daemon_restore_benchmark)

qt_add_executable(ketplus_conversation_switch_benchmark ConversationSwitchBenchmark.cpp)
target_link_libraries(ketplus_conversation_switch_benchmark PRIVATE
    Qt6::Gui ketplus_ai ketplus_daemon_core
)
ketplus_enable_warnings(ketplus_conversation_switch_benchmark)

qt_add_executable(ketplus_conversation_return_benchmark ConversationReturnBenchmark.cpp)
target_include_directories(ketplus_conversation_return_benchmark PRIVATE
    ${PROJECT_SOURCE_DIR}/apps/daemon/src
)
target_link_libraries(ketplus_conversation_return_benchmark PRIVATE
    Qt6::Quick
    Qt6::Widgets
    ketplus_ai
    ketplus_daemon_core
    KetPlusCM::ui
)
ketplus_enable_warnings(ketplus_conversation_return_benchmark)


# Needs a private daemon journal to replay, so it is never part of a default
# build: enable it explicitly when measuring transcript scroll behaviour.
option(KETPLUS_BUILD_TRANSCRIPT_SCROLL_BENCH
       "Build the transcript scroll benchmark (requires a local daemon journal to run)" OFF)
if(KETPLUS_BUILD_TRANSCRIPT_SCROLL_BENCH)
    qt_add_executable(ketplus_transcript_scroll_benchmark TranscriptScrollBenchmark.cpp)
    target_include_directories(ketplus_transcript_scroll_benchmark PRIVATE
        ${PROJECT_SOURCE_DIR}/apps/daemon/src
    )
    target_link_libraries(ketplus_transcript_scroll_benchmark PRIVATE
        Qt6::Quick
        Qt6::Widgets
        ketplus_ai
        ketplus_daemon_core
        KetPlusCM::ui
    )
    ketplus_enable_warnings(ketplus_transcript_scroll_benchmark)

    # Synthetic journal for the benchmark above, so it can run without a real one.
    qt_add_executable(ketplus_synthetic_transcript_journal SyntheticTranscriptJournal.cpp)
    target_include_directories(ketplus_synthetic_transcript_journal PRIVATE
        ${PROJECT_SOURCE_DIR}/apps/daemon/src
    )
    target_link_libraries(ketplus_synthetic_transcript_journal PRIVATE ketplus_daemon_core)
    ketplus_enable_warnings(ketplus_synthetic_transcript_journal)

endif()
