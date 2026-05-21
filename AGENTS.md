# Repository Notes

## Tests

Run the main test binary directly to see the nested test report:

```sh
build/nxt-tests
```

The report numbers every nested test, and you can select tests or whole
subtrees by number:

```sh
build/nxt-tests 1 2.7 7
```

Tests are nested with the local `_test` DSL in `test/test.hpp`.
