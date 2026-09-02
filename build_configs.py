release64 = [
    "-DCMAKE_BUILD_TYPE=Release",
    "-DALLOW_64_BIT=True",
    "-DCMAKE_CXX_FLAGS='-m64'",
    # The IRHFF experiments do not use an LP solver. Disabling optional OSI
    # discovery also prevents a host's partial COIN/CPLEX installation from
    # enabling USE_LP without providing OsiCpxSolverInterface.hpp.
    "-DCMAKE_DISABLE_FIND_PACKAGE_OSI=True",
]
debug64 = [
    "-DCMAKE_BUILD_TYPE=Debug",
    "-DALLOW_64_BIT=True",
    "-DCMAKE_CXX_FLAGS='-m64'",
    "-DCMAKE_DISABLE_FIND_PACKAGE_OSI=True",
]
asan = [
    "-DCMAKE_BUILD_TYPE=Debug",
    "-DALLOW_64_BIT=True",
    "-DCMAKE_CXX_FLAGS='-m64 -fsanitize=address -fno-omit-frame-pointer'",
    "-DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address'",
    "-DCMAKE_DISABLE_FIND_PACKAGE_OSI=True",
]
minimal = [
    "-DCMAKE_BUILD_TYPE=Release",
    "-DALLOW_64_BIT=True",
    "-DCMAKE_CXX_FLAGS='-m64'",
    "-DDISABLE_PLUGINS_BY_DEFAULT=YES",
    "-DCMAKE_DISABLE_FIND_PACKAGE_OSI=True",
]

DEFAULT = "release64"
DEBUG = "debug64"
