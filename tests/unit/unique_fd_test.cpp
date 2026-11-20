#include "unique_fd.hpp"

#include <gtest/gtest.h>

#include <fcntl.h>

using blackbox::UniqueFd;

namespace {

bool is_open(int fd)
{
    return ::fcntl(fd, F_GETFD) != -1;
}

int open_null()
{
    return ::open("/dev/null", O_RDONLY | O_CLOEXEC);
}

} // namespace

TEST(UniqueFd, ClosesOnDestruction)
{
    int raw = -1;
    {
        UniqueFd fd(open_null());
        raw = fd.get();
        ASSERT_TRUE(is_open(raw));
    }
    EXPECT_FALSE(is_open(raw));
}

TEST(UniqueFd, MoveTransfersOwnership)
{
    UniqueFd a(open_null());
    const int raw = a.get();
    UniqueFd b(std::move(a));
    EXPECT_FALSE(a);
    EXPECT_EQ(b.get(), raw);
    EXPECT_TRUE(is_open(raw));
}

TEST(UniqueFd, MoveAssignClosesPreviousDescriptor)
{
    UniqueFd a(open_null());
    UniqueFd b(open_null());
    const int old_b = b.get();
    const int raw_a = a.get();
    b = std::move(a);
    EXPECT_EQ(b.get(), raw_a);
    EXPECT_FALSE(is_open(old_b));
}

TEST(UniqueFd, ResetAndRelease)
{
    UniqueFd fd(open_null());
    const int raw = fd.release();
    EXPECT_FALSE(fd);
    EXPECT_TRUE(is_open(raw));
    fd.reset(raw);
    fd.reset();
    EXPECT_FALSE(is_open(raw));
}
