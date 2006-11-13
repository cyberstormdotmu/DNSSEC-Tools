#
# Copyright 2004 SPARTA, Inc.  All rights reserved.  See the COPYING
# file distributed with this software for details
#
# DNSSEC Tools
#
#	Keyrec file format.
#

1;

#############################################################################

=pod

=head1 NAME

B<keyrec> - Zone and key data used by DNSSEC-Tools programs.

=head1 DESCRIPTION

I<keyrec> files contain data about zones signed by and keys generated by the
DNSSEC-Tools.  A I<keyrec> file is organized in sets of I<keyrec> records.
Each I<keyrec> must be either of I<zone> type, I<set> type, or I<key> type.
Zone I<keyrec>s describe how the zones were signed.
Set I<keyrec>s describe sets of key I<keyrec>s.
Key I<keyrec>s describe how encryption keys were generated.
A I<keyrec> consists of a set of keyword/value entries.

The DNSSEC-Tools B<keyrec> module manipulates the contents of a I<keyrec>
file.  Module interfaces exist for looking up I<keyrec> records, creating
new records, and modifying existing records.

The following is an example of a zone I<keyrec>:

    zone        "example.com"
            zonefile        "db.example.com"
            signedfile      "db.example.com.signed"
            endtime         "+604800"
            kskkey          "Kexample.com.+005+33333"
            kskpath         "keydir/Kexample.com.+005+33333"
            kskdirectory    "keydir"
            zskcur          "signing-set-42"
            zskpub          "signing-set-43"
            zsknew          "signing-set-44"
            keyrec_signsecs "1123771721"
            keyrec_signdate "Thu Aug 11 14:48:41 2005"

The following is an example of a set I<keyrec>:

    set        "signing-set-42"
            zonename        "example.com"
            keys            "Kexample.com.+005+88888"
            keyrec_setsecs  "1123771350"
            keyrec_setdate  "Thu Aug 11 14:42:30 2005"

The following is an example of a key I<keyrec>:

    key        "Kexample.com.+005+88888"
            zonename        "example.com"
            algorithm       "rsasha1"
            random          "/dev/urandom"
            keypath         "./Kexample.com.+005+88888.key"
            ksklength       "1024"
            keyrec_gensecs  "1123771354"
            keyrec_gendate  "Thu Aug 11 14:42:34 2005"

=head1 COPYRIGHT
                 
Copyright 2004-2006 SPARTA, Inc.  All rights reserved.
See the COPYING file included with the DNSSEC-Tools package for details.

=head1 AUTHOR

Wayne Morrison, tewok@users.sourceforge.net

=head1 SEE ALSO

B<lskrf(1)>,
B<signset-editor(8)>,
B<zonesigner(8)>

B<Net::DNS::SEC::Tools::keyrec(3)>

=cut
