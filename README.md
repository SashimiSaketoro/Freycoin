Riecoin Core integration/staging tree
=====================================

http://www.riecoin.org

Copyright (c) 2009-2013 Bitcoin Core Developers

Copyright (c) 2013 Riecoin Developers

What is riecoin?
----------------
Riecoin is a decentralized (p2p), open source digital currency. It allows to transfer money to anywhere in the world with only minimum transaction fees, sometimes even for free (depending on many factors like the amount to transfer and the network load at the moment of the transaction). It is a fork of the Bitcoin project.

So, what is new about riecoin?

The process of money creation in Bitcoin - referred to as mining - involves executing software that utilizes your hardware running sha256 hashes until a certain criterion is met. This part of the mining process is called generating a "Proof of work". The whole mining process also has a critical role in processing transactions and providing security to the network. It is estimated that all the processing power devoted to Bitcoin mining represents more computing power than several of the largest supercomputers in the world combined. Wouldn't it be great to be able to use all that massive power for something else?

Even special purpose hardware was designed for Bitcoin mining. Some consider this a waste of resources, while others argue that supporting a decentralized currency is hardly a waste. We believe that the mining process required for currency to work does not need to include hashing functions as a Proof of Work, and that a "more useful" calculation can be done instead.

That's the point of riecoin: the mining process, besides fulfilling its function to the operation of the network, generates series of prime numbers as a by-product. This prime numbers are of interest to mathematicians and the scientific community. Riecoin is proof that it is possible to effectively harness all that massive computing power to something useful other than hashing functions.


What is Bitcoin?
----------------

Bitcoin is an experimental new digital currency that enables instant payments to
anyone, anywhere in the world. Bitcoin uses peer-to-peer technology to operate
with no central authority: managing transactions and issuing money are carried
out collectively by the network. Bitcoin Core is the name of open source
software which enables the use of this currency.

For more information, as well as an immediately useable, binary version of
the Bitcoin Core software, see http://www.bitcoin.org/en/download.

License
-------

Bitcoin Core is released under the terms of the MIT license. See [COPYING](COPYING) for more
information or see http://opensource.org/licenses/MIT.

Development process
-------------------

Developers work in their own trees, then submit pull requests when they think
their feature or bug fix is ready.

If it is a simple/trivial/non-controversial change, then one of the Bitcoin
development team members simply pulls it.

If it is a *more complicated or potentially controversial* change, then the patch
submitter will be asked to start a discussion (if they haven't already) on the
[mailing list](http://sourceforge.net/mailarchive/forum.php?forum_name=bitcoin-development).

The patch will be accepted if there is broad consensus that it is a good thing.
Developers should expect to rework and resubmit patches if the code doesn't
match the project's coding conventions (see [doc/coding.md](doc/coding.md)) or are
controversial.

The `master` branch is regularly built and tested, but is not guaranteed to be
completely stable. [Tags](https://github.com/bitcoin/bitcoin/tags) are created
regularly to indicate new official, stable release versions of Bitcoin.

Testing
-------

Testing and code review is the bottleneck for development; we get more pull
requests than we can review and test. Please be patient and help out, and
remember this is a security-critical project where any mistake might cost people
lots of money.

### Automated Testing

Developers are strongly encouraged to write unit tests for new code, and to
submit new unit tests for old code. Unit tests can be compiled and run (assuming they weren't disabled in configure) with: `make check`

Every pull request is built for both Windows and Linux on a dedicated server,
and unit and sanity tests are automatically run. The binaries produced may be
used for manual QA testing — a link to them will appear in a comment on the
pull request posted by [BitcoinPullTester](https://github.com/BitcoinPullTester). See https://github.com/TheBlueMatt/test-scripts
for the build/test scripts.

### Manual Quality Assurance (QA) Testing

Large changes should have a test plan, and should be tested by somebody other
than the developer who wrote the code.
See https://github.com/bitcoin/QA/ for how to create a test plan.
