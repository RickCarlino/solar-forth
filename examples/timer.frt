\\ One-shot timer: prints a message after 1s and exits
uv:timer 1000 0 [ drop "done" print cr bye ] uv:timer-start
uv:run
