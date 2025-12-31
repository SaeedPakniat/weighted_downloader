cmake_minimum_required(VERSION 4.0)
project(weighted_downloader C)

set(CMAKE_C_STANDARD 11)

add_executable(weighted_downloader main.c
        downloader.h
        downloader.c
        scheduler.h
        scheduler.c
        progress.c
        progress.h
        util.c
        util.h)
