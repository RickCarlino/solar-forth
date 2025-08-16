: greet "Hello from definition" print cr ;
greet

\ one-shot timers using numbers and quotations
uv:timer 500 0  [ drop "first tick" print cr ] uv:timer-start
uv:timer 1000 0 [ drop "done" print cr bye ]   uv:timer-start
uv:run
