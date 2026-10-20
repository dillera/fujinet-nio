#include "doctest.h"

#include "fujinet/io/atari_sio_transaction_boundary.h"

TEST_CASE("Atari SIO CMD edge records one host-testable transaction boundary")
{
    fujinet::io::AtariSioTransactionBoundary boundary;
    CHECK_FALSE(boundary.consume());
    boundary.note_cmd_edge();
    CHECK(boundary.consume());
    CHECK_FALSE(boundary.consume());
    boundary.note_cmd_edge();
    boundary.note_cmd_edge();
    CHECK(boundary.consume());
    CHECK_FALSE(boundary.consume());
}
