cmake_minimum_required(VERSION 3.1) 
project(MobilityDB VERSION 1.0)

set(SRCS
src/doublen.c
src/debug/indexesstat.c
src/lifting.c
src/oidcache.c
src/period.c
src/periodset.c
src/rangetypes_ext.c
src/tbox.c
src/temporal.c
src/temporali.c
src/temporalinst.c
src/temporals.c
src/temporalseq.c
src/temporal_aggfuncs.c
src/temporal_analyze.c
src/temporal_boolops.c
src/temporal_boxops.c
src/temporal_compops.c
src/temporal_gist.c
src/tnumber_mathfuncs.c
src/temporal_parser.c
src/temporal_posops.c
src/temporal_selfuncs.c
src/temporal_spgist.c
src/temporal_textfuncs.c
src/temporal_util.c
src/temporal_waggfuncs.c
src/timeops.c
src/timestampset.c
src/time_analyze.c
src/time_gist.c
src/time_selfuncs.c
src/time_spgist.c
src/tnumber_gist.c
src/tnumber_selfuncs.c
src/tnumber_spgist.c
)

set(SQL
src/sql/00_catalog.in.sql
src/sql/01_period.in.sql
src/sql/03_timestampset.in.sql
src/sql/05_periodset.in.sql
src/sql/07_rangetypes_ext.in.sql
src/sql/11_timeops.in.sql
src/sql/13_time_gist.in.sql
src/sql/15_time_spgist.in.sql
src/sql/20_doublen.in.sql
src/sql/21_tbox.in.sql
src/sql/22_temporal.in.sql
src/sql/24_tnumber_mathfuncs.in.sql
src/sql/26_temporal_boolops.in.sql
src/sql/27_temporal_textfuncs.in.sql
src/sql/28_temporal_compops.in.sql
src/sql/32_temporal_boxops.in.sql
src/sql/34_temporal_posops.in.sql
src/sql/36_temporal_aggfuncs.in.sql
src/sql/38_temporal_waggfuncs.in.sql
src/sql/40_temporal_gist.in.sql
src/sql/42_temporal_spgist.in.sql
src/sql/99_oidcache.in.sql
)

include(CTest)
enable_testing()

set(PG_REQUIRED_VERSION "PostgreSQL 11")

find_program(PGCONFIG pg_config)
if (NOT PGCONFIG)
	message(FATAL_ERROR "Could not find pg_config")
endif ()
execute_process(COMMAND ${PGCONFIG} --version OUTPUT_VARIABLE PGVERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
if (NOT "${PGVERSION}" MATCHES "${PG_REQUIRED_VERSION}")
	message(FATAL_ERROR "Wrong PostgreSQL version: found ${PGVERSION}, required ${PG_REQUIRED_VERSION}")
endif ()

execute_process(COMMAND ${PGCONFIG} --includedir --includedir-server OUTPUT_VARIABLE PostgreSQL_ACTUAL_INCLUDE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)

include_directories(${PostgreSQL_ACTUAL_INCLUDE_DIR})
include_directories("include")

add_definitions(-Wall -Wextra -std=gnu1x -Wno-unused-parameter)
if (CMAKE_COMPILER_IS_GNUCC)
	add_definitions(-Wno-misleading-indentation)
	if (WITH_COVERAGE)
		set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-arcs -ftest-coverage")
		set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fprofile-arcs -ftest-coverage")
	endif ()
endif ()

set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O0")

string(TOLOWER ${CMAKE_PROJECT_NAME} LCNAME)

set(CONTROLIN "control.in")
set(CONTROLOUT "${LCNAME}.control")
set(SQLOUT "${LCNAME}--${PROJECT_VERSION}.sql")

execute_process(COMMAND ${PGCONFIG} --sharedir OUTPUT_VARIABLE PostgreSQL_SHARE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE) # FIXME: I don't think this work on Windows
execute_process(COMMAND ${PGCONFIG} --pkglibdir OUTPUT_VARIABLE PostgreSQL_EXTLIB_DIR OUTPUT_STRIP_TRAILING_WHITESPACE) # FIXME: I don't think this work on Windows

add_custom_target(sqlscript ALL DEPENDS ${CMAKE_BINARY_DIR}/${SQLOUT})
add_custom_target(control ALL DEPENDS ${CMAKE_BINARY_DIR}/${CONTROLOUT})

add_library(${CMAKE_PROJECT_NAME} MODULE ${SRCS})

if (APPLE)
	SET_TARGET_PROPERTIES(${CMAKE_PROJECT_NAME} PROPERTIES LINK_FLAGS "-Wl,-undefined,dynamic_lookup -bundle_loader /usr/local/bin/postgres")
endif ()


find_library(HAS_LWGEOM lwgeom)

option(WITH_POSTGIS "Include PostGIS support" ON)
if (WITH_POSTGIS)
	if (HAS_LWGEOM)
		MESSAGE(STATUS "Found LWGEOM at ${HAS_LWGEOM} - compiling with PostGIS support")
	else()
		MESSAGE(STATUS "LWGEOM not found - compiling without PostGIS support")
		set(WITH_POSTGIS OFF)
	endif ()
endif ()

include("test/scripts/test.cmake")

if (WITH_POSTGIS)
		include("point/point.cmake")
endif ()

add_custom_command(
	OUTPUT ${SQLOUT}
	COMMAND mkdir -p ${CMAKE_BINARY_DIR}/sqlin
	COMMAND cp ${SQL} ${CMAKE_BINARY_DIR}/sqlin/
	COMMAND cd ${CMAKE_BINARY_DIR}/sqlin/ && find -type f | sort | xargs cat > ${CMAKE_BINARY_DIR}/${SQLOUT}
	WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
	DEPENDS ${SQL})

add_custom_command(
	OUTPUT ${CONTROLOUT}
	COMMAND cat ${CONTROLIN} | sed -e's/LIBNAME/${CMAKE_PROJECT_NAME}/'  > ${CMAKE_BINARY_DIR}/${CONTROLOUT}
	WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
	DEPENDS ${CONTROLIN})

install(TARGETS ${CMAKE_PROJECT_NAME} DESTINATION "${PostgreSQL_EXTLIB_DIR}")
install(FILES "${CMAKE_BINARY_DIR}/${CONTROLOUT}" DESTINATION "${PostgreSQL_SHARE_DIR}/extension")
install(FILES "${CMAKE_BINARY_DIR}/${SQLOUT}" DESTINATION "${PostgreSQL_SHARE_DIR}/extension")

add_custom_target(install_restart
                  $(MAKE) install
                  COMMAND systemctl restart postgresql
                  DEPENDS ${PROJECT_NAME}
                  COMMENT "Restarting PostgreSQL")


