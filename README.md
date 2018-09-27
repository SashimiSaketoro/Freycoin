Riecoin Core integration/staging tree
=====================================

http://www.riecoin.org

Copyright (c) 2009-2018 Bitcoin Core Developers

Copyright (c) 2013-2018 Riecoin Developers

What is riecoin?
----------------
Riecoin is a decentralized (p2p), open source digital currency. It allows to transfer money to anywhere in the world with only minimum transaction fees, sometimes even for free (depending on many factors like the amount to transfer and the network load at the moment of the transaction). It is a fork of the Bitcoin project.

So, what is new about riecoin?

The process of money creation in Bitcoin - referred to as mining - involves executing software that utilizes your hardware running sha256 hashes until a certain criterion is met. This part of the mining process is called generating a "Proof of work". The whole mining process also has a critical role in processing transactions and providing security to the network. It is estimated that all the processing power devoted to Bitcoin mining represents more computing power than several of the largest supercomputers in the world combined. Wouldn't it be great to be able to use all that massive power for something else?

Even special purpose hardware was designed for Bitcoin mining. Some consider this a waste of resources, while others argue that supporting a decentralized currency is hardly a waste. We believe that the mining process required for currency to work does not need to include hashing functions as a Proof of Work, and that a "more useful" calculation can be done instead.

That's the point of riecoin: the mining process, besides fulfilling its function to the operation of the network, generates series of prime numbers as a by-product. This prime numbers are of interest to mathematicians and the scientific community. Riecoin is proof that it is possible to effectively harness all that massive computing power to something useful other than hashing functions.


What is Bitcoin?
----------------

Bitcoin is an experimental digital currency that enables instant payments to
anyone, anywhere in the world. Bitcoin uses peer-to-peer technology to operate
with no central authority: managing transactions and issuing money are carried
out collectively by the network. Bitcoin Core is the name of open source
software which enables the use of this currency.

For more information, as well as an immediately useable, binary version of
the Bitcoin Core software, see https://bitcoin.org/en/download, or read the
[original whitepaper](https://bitcoincore.org/bitcoin.pdf).

License
-------

Riecoin Core is released under the terms of the MIT license. See [COPYING](COPYING) for more
information or see https://opensource.org/licenses/MIT.

Development Process
-------------------

The `master` branch is regularly built and tested, but is not guaranteed to be
completely stable. [Tags](https://github.com/bitcoin/bitcoin/tags) are created
regularly to indicate new official, stable release versions of Bitcoin Core.

The contribution workflow is described in [CONTRIBUTING.md](CONTRIBUTING.md).

The developer [mailing list](https://lists.linuxfoundation.org/mailman/listinfo/bitcoin-dev)
should be used to discuss complicated or controversial changes before working
on a patch set.

Developer IRC can be found on Freenode at #bitcoin-core-dev.

Testing
-------

Testing and code review is the bottleneck for development; we get more pull
requests than we can review and test on short notice. Please be patient and help out by testing
other people's pull requests, and remember this is a security-critical project where any mistake might cost people
lots of money.

### Automated Testing

Developers are strongly encouraged to write [unit tests](src/test/README.md) for new code, and to
submit new unit tests for old code. Unit tests can be compiled and run
(assuming they weren't disabled in configure) with: `make check`. Further details on running
and extending unit tests can be found in [/src/test/README.md](/src/test/README.md).

There are also [regression and integration tests](/test), written
in Python, that are run automatically on the build server.
These tests can be run (if the [test dependencies](/test) are installed) with: `test/functional/test_runner.py`

The Travis CI system makes sure that every pull request is built for Windows, Linux, and OS X, and that unit/sanity tests are run automatically.

### Manual Quality Assurance (QA) Testing

Changes should be tested by somebody other than the developer who wrote the
code. This is especially important for large or high-risk changes. It is useful
to add a test plan to the pull request description if testing the changes is
not straightforward.

Translations
------------

Changes to translations as well as new translations can be submitted to
[Bitcoin Core's Transifex page](https://www.transifex.com/projects/p/bitcoin/).

Translations are periodically pulled from Transifex and merged into the git repository. See the
[translation process](doc/translation_process.md) for details on how this works.

**Important**: We do not accept translation changes as GitHub pull requests because the next
pull from Transifex would automatically overwrite them again.

Translators should also subscribe to the [mailing list](https://groups.google.com/forum/#!forum/bitcoin-translators).
