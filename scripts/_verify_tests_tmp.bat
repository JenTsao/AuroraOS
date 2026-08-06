@echo off
set CXX=J:/CODE/MINGW64/bin/g++.exe
set ROOT=J:/PROJECT/auroraOS
set FLAGS=-std=c++17 -DAURORA_HOST_TEST=1 -DCONFIG_OTA_DEV_MODE=1 -DLUA_32BITS=1 -fpermissive -include %ROOT%/tests/stubs/host_prelude.hpp -I %ROOT%/tests/stubs -I %ROOT%/kernel -I %ROOT%/vfs -I %ROOT%/syscall -I %ROOT%/drivers/display -I %ROOT%/drivers/power -I %ROOT%/drivers/sensor -I %ROOT%/boards/ti/lm3s6965-qb -I %ROOT%/arch -I %ROOT%/ui -I %ROOT%/3rdparty/lua -I %ROOT%/apps -I %ROOT%

echo === test_page_allocator.cpp ===
%CXX% %FLAGS% -fsyntax-only %ROOT%/tests/unit/test_page_allocator.cpp
echo === test_mmu_pte.cpp ===
%CXX% %FLAGS% -fsyntax-only %ROOT%/tests/unit/test_mmu_pte.cpp
echo === test_mmu_manager.cpp ===
%CXX% %FLAGS% -fsyntax-only %ROOT%/tests/unit/test_mmu_manager.cpp
echo === mmu_manager.cpp (compile to obj) ===
%CXX% %FLAGS% -c %ROOT%/arch/arm/cortex-a/mmu/mmu_manager.cpp -o %ROOT%/tests/build_verify/mmu_manager.o
echo === ALL DONE ===
