<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv;

/**
 * The version of this package, which is also the version of the native library it is built for
 * (ADR-0026 section 5). A test fails when it differs from the project version in CMakeLists.txt.
 */
final class Version
{
    public const PACKAGE = '0.1.0';
}
