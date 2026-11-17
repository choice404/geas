# The geas compiler, written in C++23. One binary, geas, built from
# compiler/src with no exceptions and no RTTI, errors travel as
# std::expected values instead. GEAS_SANITIZE turns on ASan and UBSan for
# the whole build, the gate the front end and codegen suites run under
# before a change lands
cmake_minimum_required(VERSION 3.28)
project(geas LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(GEAS_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_executable(geas
    src/main.cpp
    src/source.cpp
    src/utf8.cpp
    src/escape.cpp
    src/diagnostics.cpp
    src/token.cpp
    src/duration.cpp
    src/size.cpp
    src/lexer.cpp
    src/lexnumber.cpp
    src/lexdump.cpp
    src/ast.cpp
    src/parser.cpp
    src/parseitem.cpp
    src/parsemember.cpp
    src/parsereq.cpp
    src/parseworkflow.cpp
    src/parsetype.cpp
    src/parseexpr.cpp
    src/parseprimary.cpp
    src/parsematch.cpp
    src/parsestmt.cpp
    src/astdumper.cpp
    src/astdump.cpp
    src/astdumpmember.cpp
    src/astdumpworkflow.cpp
    src/astdumpexpr.cpp
    src/astdumpstmt.cpp
    src/loader.cpp
    src/types.cpp
    src/checkattrs.cpp
    src/resolve.cpp
    src/resolved.cpp
    src/lower.cpp
    src/sigs.cpp
    src/checker.cpp
    src/checkrun.cpp
    src/checkstmt.cpp
    src/checkexpr.cpp
    src/checkctor.cpp
    src/checkcall.cpp
    src/checkinstance.cpp
    src/checkstore.cpp
    src/checkmatch.cpp
    src/checkcourse.cpp
    src/course.cpp
    src/courseexpr.cpp
    src/coursestmt.cpp
    src/coursedump.cpp
    src/checkterms.cpp
    src/checkworkflow.cpp
    src/checkamends.cpp
    src/checkamendcourse.cpp
    src/amendshape.cpp
    src/amenddiff.cpp
    src/amendmap.cpp
    src/typecode.cpp
    src/reqsolve.cpp
    src/reqsat.cpp
    src/reqselftest.cpp
    src/hash.cpp
    src/shape.cpp
    src/shaperecord.cpp
    src/layout.cpp
    src/layoutcode.cpp
    src/check.cpp
    src/emittext.cpp
    src/emitlookup.cpp
    src/emitc.cpp
    src/emitmodule.cpp
    src/emitdesc.cpp
    src/emitflow.cpp
    src/emitremedy.cpp
    src/emitreq.cpp
    src/emitstmt.cpp
    src/emitassign.cpp
    src/emitexpr.cpp
    src/emitvalue.cpp
    src/emitcall.cpp
    src/emitstore.cpp
    src/emitstorequery.cpp
    src/emitmatch.cpp
    src/emitworkflow.cpp
    src/emitcourse.cpp
    src/emitamends.cpp
    src/emittypes.cpp
    src/typetable.cpp
    src/typedump.cpp
    src/emitcoursestep.cpp
    src/emitcoursematch.cpp
    src/emitheader.cpp
    src/emitbin.cpp
    src/emitproto.cpp
    src/emitprotosignal.cpp
    src/emitprototype.cpp
    src/emitprotodecl.cpp
    src/emitprotomsg.cpp
    src/emitsession.cpp
    src/build.cpp
    src/record.cpp
)

target_compile_options(geas PRIVATE
    -Wall -Wextra -Wpedantic
    -fno-exceptions -fno-rtti
)

if(GEAS_SANITIZE)
    target_compile_options(geas PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
    )
    target_link_options(geas PRIVATE -fsanitize=address,undefined)
endif()
