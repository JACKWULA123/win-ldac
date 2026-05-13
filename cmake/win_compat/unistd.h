// Stub <unistd.h> for MSVC. libldac's ldacBT_internal.h includes <unistd.h>
// but the symbols it actually uses (size_t, etc.) are already provided via
// other libldac includes. We just need the include to succeed.
#pragma once
