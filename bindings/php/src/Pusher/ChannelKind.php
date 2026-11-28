<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Pusher;

/** How a channel is protected, decided by its name prefix alone. */
enum ChannelKind
{
    /** Anyone may subscribe. */
    case Public;
    /** A signature from the application proves the subscriber may join. */
    case Private;
    /** Like Private, and the subscriber is also a visible member with an id. */
    case Presence;
}
