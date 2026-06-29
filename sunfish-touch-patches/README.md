# sunfish touchscreen — the bits missing from the heidelberg FTS5 series

These 5 patches sit **on top of** `sunfish-mainline` + the heidelberg stmfts5 v4
series. They add the sunfish-specific glue that the upstream FTS5 series doesn't
carry: the gpio/consumer.h include, the FTS5 event-ID mask, the DRM
panel-follower (the Pixel 4a touch IC is in-cell and powered off the panel
rails), and the two sunfish DT changes.

Apply them in your existing build tree:

    git am sunfish-touch-patches/00*.patch

Verified to `git am` clean on `sunfish-mainline` + heidelberg v4.
