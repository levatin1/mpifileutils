# FileUtils DevOps
This directory contains several files which will allow anyone to create a
consistent virtual development environment with a few simple commands.

## Installation
First, install the following software packages. Most of these packages may be
easily installed using your local package manager (brew, yum/epel, apt, etc).

* [Ansible](http://docs.ansible.com/intro_installation.html)
* [Vagrant](http://www.vagrantup.com/)
* [VirtualBox](https://www.virtualbox.org/)

## Quick Start
After dependencies are installed, perform the following actions in a shell to
enter a virtual machine with fileutils installed.
```
 cd fileutils/devops
 vagrant up
 vagrant ssh
```
