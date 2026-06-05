#lang forge

option run_sterling off

sig A {}

run {
  true
}

test expect {
  true is sat
}
