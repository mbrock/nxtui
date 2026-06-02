# RFC 0000: Prolegomena to NXT System Theory

## Status

Exploratory.

## Summary

This document introduces a small vocabulary for understanding the recurring
structures that appear throughout nxtrt.

The goal is not to prescribe implementation techniques. The goal is to
identify a handful of structural distinctions that appear repeatedly in
scheduling, buffering, ownership, streaming, framing, I/O, and structured
concurrency.

The central thesis is that many apparently unrelated runtime mechanisms are
manifestations of the same underlying patterns.

## SNAP and SPAN

Following Barry Smith, we distinguish:

- SNAP descriptions: what exists
- SPAN descriptions: what happens

Examples:

```text
firm
wand
buffer
hive
land
```

are SNAP structures.

```text
feed
stream
event log
completion queue
```

are SPAN structures.

A SNAP structure describes occupancy.

A SPAN structure describes becoming.

Many runtime systems can be understood as transformations between the two.

## Hives and Rings

The two primary storage topologies are hives and rings.

A ring is organized by succession:

```text
A -> B -> C -> D
```

Position is identity.

A hive is organized by occupancy:

```text
cell 17
cell 22
cell 91
```

Identity survives movement, completion, and reordering.

Rings naturally support feeds.

Hives naturally support firms and wand state.

A completion stream is often the SPAN projection of a hive.

## Feeds and Firms

The two fundamental runtime abstractions are:

```text
feed
firm
```

A feed is an ordered becoming.

A firm is an accountable scope.

A feed answers:

> what is happening?

A firm answers:

> who undertakes what is happening?

Feeds are horizontal.

Firms are vertical.

Feeds are SPAN.

Firms are SNAP.

## Ownership and Claims

Ownership is prior to verification.

A claim may exist whether or not it is enforced, observed, or statically
proven.

Examples:

```text
buffer ownership
task ownership
deeds
borrows
leases
reservations
```

are all claim structures.

Different systems vary primarily in how those claims are verified.

## Buffer Royalty

Buffers are not implementation details.

Buffers are territories.

A buffer grants a right to hold state between cause and effect.

Higher-level abstractions frequently derive their power from lower-level
buffers.

The layer that owns the buffer pays the holding cost.

Other layers borrow the resulting affordances.

## Buffers as Land

Memory is raw land.

Buffers are surveyed parcels.

A feed buffer is not merely storage.

It is a niche with specific affordances:

- lookahead
- ownership
- borrowing
- batching
- framing

A buffer is therefore not merely geometry.

It is geometry plus rights.

## Niches

A niche is a structured environment supporting certain activities.

Tasks inhabit firms.

Wishes inhabit wands.

Values inhabit feeds.

Buffers form niches for computation.

A niche is neither a container nor an occupant.

It is the structured relation between them.

## Occurrent Interpretation Layers

A reel is not a storage ontology.

A reel is an interpretation layer over a feed.

```text
feed<Stock>
    ↓
chop
    ↓
reel
```

The feed owns stock.

The chop surveys visible stock.

The reel advances causally through frame boundaries.

The reel changes the unit of observation without introducing a new substrate.

## Guiding Principle

When introducing a new abstraction, first ask:

What is being held?

Who has a claim on it?

What causes release?

Where is the niche in which it exists?

Only then ask:

How is it scheduled?

How is it verified?

How is it optimized?
