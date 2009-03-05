#
# First batch of Q construct tests:
# Feed packets from a verified client
#

use Test;
BEGIN { plan tests => 27 };
use runproduct;
use istest;
use Ham::APRS::IS;
ok(1); # If we made it this far, we're ok.

my $p = new runproduct('basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");


my $login = "N0CALL-1";
my $server_call = "TESTING";
my $i_tx = new Ham::APRS::IS("localhost:14581", $login);
ok(defined $i_tx, 1, "Failed to initialize Ham::APRS::IS");

my $i_rx = new Ham::APRS::IS("localhost:10152", "N0CALL-2");
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

# connect, initially to the client-only port 14581

my $ret;
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});
$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

# do the actual tests

#
# All packets
# {
#    Place into TNC-2 format
#    If a q construct is last in the path (no call following the qPT)
#       delete the qPT
# }
#  ... and will continue to add qAO
#

istest::txrx(\&ok, $i_tx, $i_rx,
	"SRC>DST,DIGI1,DIGI5*,qAR:a4ufy",
	"SRC>DST,DIGI1,DIGI5*,qAO,$login:a4ufy");

#
#    If the packet entered the server from a verified client-only connection AND the FROMCALL does not match the login:
#    {
#        if a q construct exists in the packet
#            if the q construct is at the end of the path AND it equals ,qAR,login
#                (1) Replace qAR with qAo
#            (5) else: skip to "all packets with q constructs")
#        else if the path is terminated with ,I
#        {
#            if the path is terminated with ,login,I
#                (2) Replace ,login,I with qAo,login
#            else
#                (3) Replace ,VIACALL,I with qAr,VIACALL
#        }
#        else
#            (4) Append ,qAO,login
#        Skip to "All packets with q constructs"
#    }
#    

# (1)
istest::txrx(\&ok, $i_tx, $i_rx,
	"SRCCALL>DST,DIGI1*,qAR,$login:testing (1)",
	"SRCCALL>DST,DIGI1*,qAo,$login:testing (1)");
# (2)
istest::txrx(\&ok, $i_tx, $i_rx,
	"SRCCALL>DST,DIGI1*,$login,I:testing (2)",
	"SRCCALL>DST,DIGI1*,qAo,$login:testing (2)");
# (3)
istest::txrx(\&ok, $i_tx, $i_rx,
	"SRCCALL>DST,DIGI1*,IGATE,I:testing (3)",
	"SRCCALL>DST,DIGI1*,qAr,IGATE:testing (3)");
# (4)
istest::txrx(\&ok, $i_tx, $i_rx,
	"SRCCALL>DST,DIGI1*:testing (4)",
	"SRCCALL>DST,DIGI1*,qAO,$login:testing (4)");

# (5) - any other (even unknown) q construct is passed intact
istest::txrx(\&ok, $i_tx, $i_rx,
	"SRCCALL>DST,DIGI1*,qAF,$login:testing (5)",
	"SRCCALL>DST,DIGI1*,qAF,$login:testing (5)");

#
# reconnect to the non-client-only port
#

$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});
$i_tx = new Ham::APRS::IS("localhost:10152", $login);
ok(defined $i_tx, 1, "Failed to initialize Ham::APRS::IS");
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});

# for loop testing, also make a second connection
my $login_second = "MYC4LL-5";
$i_tx2 = new Ham::APRS::IS("localhost:10152", $login_second);
ok(defined $i_tx2, 1, "Failed to initialize Ham::APRS::IS");
$ret = $i_tx2->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect twice to the server: " . $i_tx->{'error'});

#
#    If a q construct exists in the header:
#        (a1) Skip to "All packets with q constructs"
#
# Hmm, javaprssrvr doesn't seem to implement this, goes to the qAC path

#istest::txrx(\&ok, $i_tx, $i_rx,
#	"$login>DST,DIGI1*,qAR,$login:testing (a1)",
#	"$login>DST,DIGI1*,qAR,$login:testing (a1)");


#    If header is terminated with ,I:
#    {
#        If the VIACALL preceding the ,I matches the login:
#            (b1) Change from ,VIACALL,I to ,qAR,VIACALL
#        Else
#            (b2) Change from ,VIACALL,I to ,qAr,VIACALL
#    }

istest::txrx(\&ok, $i_tx, $i_rx,
	"SRC>DST,DIGI1,DIGI5*,$login,I:Asdf (b1)",
	"SRC>DST,DIGI1,DIGI5*,qAR,$login:Asdf (b1)");

istest::txrx(\&ok, $i_tx, $i_rx,
	"SRC>DST,DIGI1,DIGI5*,N0CALL,I:Asdf (b2)",
	"SRC>DST,DIGI1,DIGI5*,qAr,N0CALL:Asdf (b2)");

#
#    Else If the FROMCALL matches the login:
#    {
#        Append ,qAC,SERVERLOGIN
#        Quit q processing
#    }
#    Else
#        Append ,qAS,login
#    Skip to "All packets with q constructs"
#

istest::txrx(\&ok, $i_tx, $i_rx,
	"$login>DST:aifyua",
	"$login>DST,TCPIP*,qAC,$server_call:aifyua");

istest::txrx(\&ok, $i_tx, $i_rx,
	"SRC>DST,DIGI1,DIGI2*:test",
	"SRC>DST,DIGI1,DIGI2*,qAS,$login:test");


#
# All packets with q constructs:
# {
#     if ,qAZ, is the q construct:
#     {
#         Dump to the packet to the reject log
#         Quit processing the packet
#     }
#

$i_tx->sendline("SRCCALL>DST,DIGI1*,qAZ,$login:testing (qAZ)");

#
#     If ,SERVERLOGIN is found after the q construct:
#     {
#         Dump to the loop log with the sender's IP address for identification
#         Quit processing the packet
#     }

$i_tx->sendline("SRCCALL>DST,DIGI1*,qAZ,$login:testing (,SERVERLOGIN)");

#
#    If a callsign-SSID is found twice in the q construct:
#    {
#        Dump to the loop log with the sender's IP address for identification
#        Quit processing the packet
#    }
#

$i_tx->sendline("SRCCALL>DST,DIGI1*,qAI,FOOBAR,ASDF,ASDF,BARFOO:testing (dup call)");

#
#    If a verified login other than this login is found in the q construct
#    and that login is not allowed to have multiple verified connects (the
#    IPADDR of an outbound connection is considered a verified login):
#    {
#        Dump to the loop log with the sender's IP address for identification
#        Quit processing the packet
#    }
#
# (to test this, we made a second connection using call $login_second)
$i_tx->sendline("SRCCALL>DST,DIGI*,qAI,$login_second,$login:testing (verified call loop)");

#
#    If the packet is from an inbound port and the login is found after the q construct but is not the LAST VIACALL:
#    {
#        Dump to the loop log with the sender's IP address for identification
#        Quit processing the packet
#    }
#
$i_tx->sendline("SRCCALL>DST,DIGI*,qAI,$login,M0RE:testing (login not last viacall)");

#
#    If trace is on, the q construct is qAI, or the FROMCALL is on the server's trace list:
#    {
#        If the packet is from a verified port where the login is not found after the q construct:
#            (1) Append ,login
#        else if the packet is from an outbound connection
#            (2) Append ,IPADDR
#
#        (3) Append ,SERVERLOGIN
#    }
#

# (1):
istest::txrx(\&ok, $i_tx, $i_rx,
	"SRC>DST,DIGI1,DIGI2*,qAI,FOOBAR:testing qAI (1)",
	"SRC>DST,DIGI1,DIGI2*,qAI,FOOBAR,$login,$server_call:testing qAI (1)");

# (2) needs to be tested elsewhere
# (3):
istest::txrx(\&ok, $i_tx, $i_rx,
	"SRC>DST,DIGI1,DIGI2*,qAI,$login:testing qAI (3)",
	"SRC>DST,DIGI1,DIGI2*,qAI,$login,$server_call:testing qAI (3)");

# disconnect

$ret = $i_rx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});
$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_tx->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

