"""walk_race.py: two replicas over one park store, raced for the same tokens
round after round.

The walk parks one payment session per round on the peer replica, then for
each token starts a Resume on both replicas at the same moment behind a
barrier. The runtime's claim resumes the instance and deletes its row in one
move, so every round has exactly one replica holding the contract and the
other answering NOT_FOUND, the same answer a spent token gets. The first
round's winner finishes the parked walk, so the claimed instance is the real
one; every other winner closes its stream, which parks the token again.

An ended id answers NOT_FOUND only once its ending is over, its park row
written, so every token is in the store before the first round starts and a
round measures the claim and nothing else.
"""

import threading
from concurrent import futures

import grpc

from clientkit import Session, channel_ready, wait_gone

# Rounds raced when the caller names none
DEFAULT_ROUNDS = 8


def park_one(stub, pb):
    """Signs a payment session, latches one pledge, closes the stream, and
    answers the park token."""
    with Session(stub, pb.SignRequest(currency="EUR")) as session:
        stub.ValidateCard(pb.ValidateCardRequest(
            instance_id=session.instance_id, card="4111 1111"))
        token = session.signed.park_token
        instance_id = session.instance_id
    if wait_gone(stub, pb.PartialRequest, instance_id, 5.0) is None:
        raise RuntimeError(f"session {instance_id} never ended")
    return token


def _race(pb, stubs, token):
    """One round: both replicas resume token at once. Answers the winners
    as (stub, Session) pairs and the losers as RpcErrors."""
    barrier = threading.Barrier(len(stubs))

    def attempt(one_stub):
        barrier.wait()
        try:
            return Session(one_stub, pb.ResumeRequest(park_token=token),
                           one_stub.Resume)
        except grpc.RpcError as e:
            return e

    with futures.ThreadPoolExecutor(max_workers=len(stubs)) as pool:
        results = list(zip(stubs, pool.map(attempt, stubs)))
    winners = [(s, r) for s, r in results if isinstance(r, Session)]
    losers = [r for _, r in results if isinstance(r, grpc.RpcError)]
    return winners, losers


def _round(pb, stubs, token, index, tally, finish):
    winners, losers = _race(pb, stubs, token)
    is_clean = (len(winners) == 1 and len(losers) == 1 and
                losers[0].code() == grpc.StatusCode.NOT_FOUND)
    codes = [e.code().name for e in losers]
    tally.check(is_clean, f"round {index}: one replica claimed the token, "
                f"got {len(winners)} winners and losers {codes}")
    for winner_stub, session in winners:
        if index == 0 and is_clean:
            finish(winner_stub, pb, session.instance_id, "raced")
        session.close()
    return is_clean


def walk(pb, pb_grpc, ports, rounds, tally, finish):
    """Parks rounds tokens on the second port and races both ports for each."""
    channels = [channel_ready(p, tally) for p in ports]
    try:
        if None in channels:
            return
        stubs = [pb_grpc.PaymentServiceStub(ch) for ch in channels]
        tokens = [park_one(stubs[1], pb) for _ in range(rounds)]
        clean = sum(_round(pb, stubs, token, i, tally, finish)
                    for i, token in enumerate(tokens))
        print(f"[bridge_client] {clean} of {rounds} raced rounds had one claim")
    finally:
        for channel in channels:
            if channel is not None:
                channel.close()
