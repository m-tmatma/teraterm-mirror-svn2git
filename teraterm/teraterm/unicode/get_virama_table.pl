#!/usr/bin/perl
use strict;
use warnings;
use utf8;

my $in_file = "UnicodeData.txt";
my $out_file = "unicode_virama.tbl";
my $IN;
my $OUT;
open($IN, $in_file) || die "Cannot open $in_file.";
open($OUT, ">:crlf:encoding(utf8)", $out_file) || die "Cannot open $out_file.";
print $OUT "// this file was generated by get_virama_table.pl\n";
my $ostart = 0;
my $oend = 0;
my $ocanonical_combining_class = 0;
my $combine = 0;
my $comment;
while($a = <$IN>) {
	if ($a =~ /^([0-9A-F]+);([^;]*);[^;]*;([^;]*);/) {
		my $code_point = hex $1;
		my $name = $2;
		my $canonical_combining_class = $3;

		#printf("%06x| %s| \"%s\"\n", $code_point, $general_category, $name);

		$combine = 0;
		if ($canonical_combining_class == 9) {
			# いまのところ 9 のみ対応
			$combine = 1;
		}

		my $output = 0;
		if ($combine == 1) {
			if ($ostart == 0) {
				$output = 0;
			} else {
				if ($ocanonical_combining_class ne $canonical_combining_class) {
					$output = 1;
				}
			}
		} else {
			if ($ostart != 0) {
				$output = 1;
			}
		}

		if ($output != 0) {
			#printf $OUT "{ 0x%06x, 0x%06x, %s },		// '%s'\n", $ostart, $oend, $ocanonical_combining_class, $comment;
			printf $OUT "{ 0x%06x, 0x%06x },		// '%s'\n", $ostart, $oend, $comment;
			$ostart = 0;
		}

		if ($combine == 1) {
			if ($ostart == 0) {
				$ostart = $code_point;
				$ocanonical_combining_class = $canonical_combining_class;
				$comment = $name;
			}
			$oend = $code_point;
		}
	}
}
if ($ostart != 0) {
	printf $OUT "{ 0x%06x, 0x%06x },\n", $ostart, $oend;
}