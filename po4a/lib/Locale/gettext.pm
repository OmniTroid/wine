#!/usr/bin/perl
# (c) Copyright 2004. CodeWeavers, Inc.
# Dummy gettext Perl module for systems that don't have
# Locale::gettext installed

package Locale::gettext;

use strict;

# Define the module interface
use vars qw(@ISA @EXPORT);
use Exporter ();
@ISA    = "Exporter";
@EXPORT = qw(dgettext
             gettext
             textdomain
            );

sub textdomain($)
{
    # Do nothing
}

sub gettext(@)
{
    return join("",@_);
}

sub dgettext(@)
{
    shift @_;
    return join("",@_);
}
