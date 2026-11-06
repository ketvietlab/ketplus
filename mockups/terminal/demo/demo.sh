#!/bin/sh
# Sample output for review captures: ANSI colours, a Git summary and a test run.
clear
printf '\033]0;ketplus — demo\007'
b='\033[1m'; d='\033[2m'; r='\033[0m'
g='\033[32m'; y='\033[33m'; c='\033[36m'; m='\033[35m'; red='\033[31m'; bl='\033[34m'
printf "${d}\$${r} git status --short --branch\n"
printf "${g}## develop${r}...${red}origin/develop${r} [ahead 2]\n"
printf " ${red}M${r} core/src/terminal/TerminalView.cpp\n"
printf " ${red}M${r} mockups/terminal/src/TerminalMockWindow.cpp\n"
printf "${red}??${r} mockups/terminal/demo/\n\n"
printf "${d}\$${r} ls\n"
printf "${bl}${b}apps${r}      ${bl}${b}core${r}      ${bl}${b}docs${r}      ${bl}${b}mockups${r}   CMakeLists.txt   README.md\n\n"
printf "${d}\$${r} ctest --test-dir build -R terminal\n"
printf "    Start 12: terminal_session\n"
printf "1/3 Test #12: terminal_session ...........   ${g}Passed${r}    0.41 sec\n"
printf "2/3 Test #13: terminal_view ..............   ${g}Passed${r}    0.88 sec\n"
printf "3/3 Test #14: terminal_app ...............   ${g}Passed${r}    1.02 sec\n\n"
printf "${g}100%% tests passed${r}, 0 tests failed out of 3\n\n"
printf "${c}info${r}  ${y}warn${r}  ${red}error${r}  ${m}magenta${r}  ${b}bold${r}  ${d}dim${r}  \033[4munderline${r}  \033[7m reverse ${r}\n\n"
