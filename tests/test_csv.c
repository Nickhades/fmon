#include "csv_util.h"

#include <assert.h>
#include <string.h>

static void check(const char *in, const char *want)
{
    char *got = fmon_csv_escape(in);

    assert(g_strcmp0(got, want) == 0);
    g_free(got);
}

int main(void)
{
    check("abc", "\"abc\"");
    check("", "\"\"");
    check("a,b", "\"a,b\"");
    check("a\"b", "\"a\"\"b\"");
    check("a\nb", "\"a\nb\"");
    check("=SUM(A1:A2)", "\"'=SUM(A1:A2)\"");
    check("+cmd", "\"'+cmd\"");
    check("-cmd", "\"'-cmd\"");
    check("@cmd", "\"'@cmd\"");
    return 0;
}
