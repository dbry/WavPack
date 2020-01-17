#!/bin/bash

if ./check-address fuzzing/regression/* && ./check-memory fuzzing/regression/* && ./check-undefined fuzzing/regression/*
then
    echo all regression checks ran to completion
    exit 0
else
    echo at least one regression test failed
    exit 1
fi
