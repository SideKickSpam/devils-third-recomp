@echo off
rem Like dt-port.cmd, with the sampling profiler on: it records where the game and rendering threads spend their time,
rem frame by frame, in build\profile.sample (about 5 MB a minute), so slow stretches (stutter, frame dips) can be traced
rem to the code that causes them. It costs a little smoothness; use it for a session meant for diagnosis.
call "%~dp0dt-port.cmd" --sample build\profile.sample --sample-threads "Game Thread,RenderingThread,GPU" %*
