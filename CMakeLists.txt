PROJECT (df-ai)

SET(PROJECT_SRCS
    df-ai.cpp
    ai.cpp
    config.cpp
    population.cpp
    plan.cpp
    plan_blueprint.cpp
    stocks.cpp
    camera.cpp
    embark.cpp
    room.cpp
    event_manager.cpp
)

SET(PROJECT_HDRS
    ai.h
    config.h
    population.h
    plan.h
    plan_blueprint.h
    stocks.h
    camera.h
    embark.h
    room.h
    event_manager.h
    dfhack_shared.h
)

SET_SOURCE_FILES_PROPERTIES( ${PROJECT_HDRS} PROPERTIES HEADER_FILE_ONLY TRUE)

LIST(APPEND PROJECT_SRCS ${PROJECT_HDRS})

LIST(APPEND PROJECT_LIBS jsoncpp)

DFHACK_PLUGIN(df-ai ${PROJECT_SRCS} LINK_LIBRARIES ${PROJECT_LIBS} COMPILE_FLAGS_GCC "-Wall -Wextra -Werror" COMPILE_FLAGS_MSVC "/W3")

# vim: et:sw=4:ts=4
