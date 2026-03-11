cmake_minimum_required(VERSION 3.5)

set(COMPONENTS "main retro-go quake")
set(RG_ENABLE_NETWORKING 0 CACHE INTERNAL "" FORCE)
set(RG_ENABLE_NETPLAY 0 CACHE INTERNAL "" FORCE)
include(../base.cmake)
project(quake-go)
