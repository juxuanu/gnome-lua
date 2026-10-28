/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * credential-manager.h: Sources of preauthenticated credentials of the login
 * and lock screens. Port of js/gdm/credentialManager.js, js/gdm/oVirt.js and
 * js/gdm/vmware.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * SHELL_OVIRT_SERVICE_NAME:
 *
 * The PAM service that takes the oVirt token (`Shell.OVIRT_SERVICE_NAME`).
 */
#define SHELL_OVIRT_SERVICE_NAME "gdm-ovirtcred"

/**
 * SHELL_VMWARE_SERVICE_NAME:
 *
 * The PAM service that takes the VMware token (`Shell.VMWARE_SERVICE_NAME`).
 */
#define SHELL_VMWARE_SERVICE_NAME "gdm-vmwcred"

#define SHELL_TYPE_CREDENTIAL_MANAGER (shell_credential_manager_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellCredentialManager, shell_credential_manager,
                          SHELL, CREDENTIAL_MANAGER, GObject)

struct _ShellCredentialManagerClass
{
  GObjectClass parent_class;
};

const char *shell_credential_manager_get_token   (ShellCredentialManager *manager);
void        shell_credential_manager_set_token   (ShellCredentialManager *manager,
                                                  const char             *token);
const char *shell_credential_manager_get_service (ShellCredentialManager *manager);


#define SHELL_TYPE_OVIRT_CREDENTIAL_MANAGER (shell_ovirt_credential_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellOvirtCredentialManager, shell_ovirt_credential_manager,
                      SHELL, OVIRT_CREDENTIAL_MANAGER, ShellCredentialManager)

ShellOvirtCredentialManager *shell_ovirt_credential_manager_get_default (void);


#define SHELL_TYPE_VMWARE_CREDENTIAL_MANAGER (shell_vmware_credential_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellVmwareCredentialManager, shell_vmware_credential_manager,
                      SHELL, VMWARE_CREDENTIAL_MANAGER, ShellCredentialManager)

ShellVmwareCredentialManager *shell_vmware_credential_manager_get_default (void);

G_END_DECLS
