/*
 * Copyright 2023 Robert Tari <robert@tari.in>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <glib/gi18n.h>
#include <gio/gio.h>
#include <act/act.h>
#include "service.h"

#define BUS_NAME "org.ayatana.indicator.a11y"
#define BUS_PATH "/org/ayatana/indicator/a11y"
#define GREETER_BUS_NAME "org.ayatana.greeter"
#define GREETER_BUS_PATH "/org/ayatana/greeter"
#define GREETER_SETTINGS "org.ArcticaProject.arctica-greeter"

static guint m_nSignal = 0;

struct _IndicatorA11yServicePrivate
{
    guint nOwnId;
    guint nActionsId;
    GDBusConnection *pConnection;
    GSimpleActionGroup *pActionGroup;
    GMenu *pMenu;
    GMenu *pSubmenu;
    guint nExportId;
    GSimpleAction *pHeaderAction;
    guint nOnboardSubscription;
    gboolean bOnboardActive;
    GSettings *pOrcaSettings;
    guint nOrcaSubscription;
    gboolean bOrcaActive;
    gboolean bHighContrast;
    GSettings *pHighContrastSettings;
    gboolean bIgnoreSettings;
    gchar *sThemeGtk;
    gchar *sThemeIcon;
    gboolean bGreeter;
    GSList *lUsers;
    gchar *sUser;
    guint nGreeterSubscription;
};

typedef IndicatorA11yServicePrivate priv_t;

G_DEFINE_TYPE_WITH_PRIVATE (IndicatorA11yService, indicator_a11y_service, G_TYPE_OBJECT)

static GVariant* createHeaderState (IndicatorA11yService *self)
{
    GVariantBuilder cBuilder;
    g_variant_builder_init (&cBuilder, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&cBuilder, "{sv}", "title", g_variant_new_string (_("Accessibility")));
    g_variant_builder_add (&cBuilder, "{sv}", "tooltip", g_variant_new_string (_("Accessibility settings")));
    g_variant_builder_add (&cBuilder, "{sv}", "visible", g_variant_new_boolean (TRUE));
    g_variant_builder_add (&cBuilder, "{sv}", "accessible-desc", g_variant_new_string (_("Accessibility settings")));

    GIcon *pIcon = g_themed_icon_new_with_default_fallbacks ("preferences-desktop-accessibility-panel");
    GVariant *pSerialized = g_icon_serialize (pIcon);

    if (pSerialized != NULL)
    {
        g_variant_builder_add (&cBuilder, "{sv}", "icon", pSerialized);
        g_variant_unref (pSerialized);
    }

    g_object_unref (pIcon);

    return g_variant_builder_end (&cBuilder);
}

static void onOnboardBus (GDBusConnection *pConnection, const gchar *sSender, const gchar *sPath, const gchar *sInterface, const gchar *sSignal, GVariant *pParameters, gpointer pUserData)
{
    GVariant *pDict = g_variant_get_child_value (pParameters, 1);
    GVariant* pValue = g_variant_lookup_value (pDict, "Visible", G_VARIANT_TYPE_BOOLEAN);
    g_variant_unref (pDict);

    IndicatorA11yService *self = INDICATOR_A11Y_SERVICE (pUserData);
    gboolean bActive = g_variant_get_boolean (pValue);

    if (bActive != self->pPrivate->bOnboardActive)
    {
        self->pPrivate->bOnboardActive = bActive;
        GAction *pAction = g_action_map_lookup_action (G_ACTION_MAP (self->pPrivate->pActionGroup), "onboard");
        g_action_change_state (pAction, pValue);
    }

    g_variant_unref (pValue);
}

static void onUserLoaded (IndicatorA11yService *self, ActUser *pUser)
{
    g_signal_handlers_disconnect_by_func (G_OBJECT (pUser), G_CALLBACK (onUserLoaded), self);

    if (!self->pPrivate->sUser)
    {
        GError *pError = NULL;
        GVariant *pUser = g_dbus_connection_call_sync (self->pPrivate->pConnection, GREETER_BUS_NAME, GREETER_BUS_PATH, GREETER_BUS_NAME, "GetUser", NULL, G_VARIANT_TYPE ("(s)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &pError);

        if (pError)
        {
            g_debug ("Failed calling GetUser: %s", pError->message);
            g_error_free (pError);

            return;
        }

        g_variant_get (pUser, "(s)", &self->pPrivate->sUser);
    }

    gboolean bPrefix = g_str_has_prefix (self->pPrivate->sUser, "*");

    if (!bPrefix)
    {
        const gchar *sUser = act_user_get_user_name (pUser);
        gboolean bSame = g_str_equal (self->pPrivate->sUser, sUser);

        if (bSame)
        {
            gint nUid = act_user_get_uid (pUser);
            gchar *sPath = g_strdup_printf ("/org/freedesktop/Accounts/User%i", nUid);
            GDBusConnection *pConnection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
            GDBusProxy *pProxy = g_dbus_proxy_new_sync (pConnection, G_DBUS_PROXY_FLAGS_NONE, NULL, "org.freedesktop.Accounts", sPath, "org.freedesktop.DBus.Properties", NULL, NULL);
            GVariant *pOrcaValue = g_variant_new ("b", self->pPrivate->bOrcaActive);
            GVariant *pOrcaParams = g_variant_new ("(ssv)", "org.ayatana.indicator.a11y.AccountsService", "OrcaEnabled", pOrcaValue);
            GVariant *pOrcaRet = g_dbus_proxy_call_sync (pProxy, "Set", pOrcaParams, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
            g_variant_unref (pOrcaRet);
            GVariant *pOnboardValue = g_variant_new ("b", self->pPrivate->bOnboardActive);
            GVariant *pOnboardParams = g_variant_new ("(ssv)", "org.ayatana.indicator.a11y.AccountsService", "OnBoardEnabled", pOnboardValue);
            GVariant *pOnboardRet = g_dbus_proxy_call_sync (pProxy, "Set", pOnboardParams, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
            g_variant_unref (pOnboardRet);
            GVariant *pContrastValue = g_variant_new ("b", self->pPrivate->bHighContrast);
            GVariant *pContrastParams = g_variant_new ("(ssv)", "org.ayatana.indicator.a11y.AccountsService", "HighContrastEnabled", pContrastValue);
            GVariant *pContrastRet = g_dbus_proxy_call_sync (pProxy, "Set", pContrastParams, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
            g_variant_unref (pContrastRet);
            g_object_unref (pConnection);
            g_free (sPath);
        }
    }
}

static void onManagerLoaded (IndicatorA11yService *self)
{
    ActUserManager *pManager = act_user_manager_get_default ();

    if (!self->pPrivate->lUsers)
    {
        self->pPrivate->lUsers = act_user_manager_list_users (pManager);
    }

    for (GSList *lUser = self->pPrivate->lUsers; lUser; lUser = lUser->next)
    {
        ActUser *pUser = lUser->data;
        gboolean bLoaded = act_user_is_loaded (pUser);

        if (bLoaded)
        {
            onUserLoaded (self, pUser);
        }
        else
        {
            g_signal_connect_swapped (pUser, "notify::is-loaded", G_CALLBACK (onUserLoaded), self);
        }
    }
}

static void loadManager (IndicatorA11yService *self)
{
    ActUserManager *pManager = act_user_manager_get_default ();
    gboolean bLoaded = FALSE;
    g_object_get (pManager, "is-loaded", &bLoaded, NULL);

    if (bLoaded)
    {
        onManagerLoaded (self);
    }
    else
    {
        g_signal_connect_object (pManager, "notify::is-loaded", G_CALLBACK (onManagerLoaded), self, G_CONNECT_SWAPPED);
    }
}

static void onUserChanged (GDBusConnection *pConnection, const gchar *sSender, const gchar *sPath, const gchar *sInterface, const gchar *sSignal, GVariant *pParameters, gpointer pUserData)
{
    IndicatorA11yService *self = INDICATOR_A11Y_SERVICE (pUserData);
    g_variant_get (pParameters, "(s)", &self->pPrivate->sUser);
    loadManager (self);
}

static void onBusAcquired (GDBusConnection *pConnection, const gchar *sName, gpointer pData)
{
    IndicatorA11yService *self = INDICATOR_A11Y_SERVICE (pData);

    g_debug ("bus acquired: %s", sName);

    GError *pError = NULL;
    self->pPrivate->nActionsId = g_dbus_connection_export_action_group (pConnection, BUS_PATH, G_ACTION_GROUP (self->pPrivate->pActionGroup), &pError);

    // Export the actions
    if (!self->pPrivate->nActionsId)
    {
        g_warning ("cannot export action group: %s", pError->message);
        g_clear_error(&pError);
    }

    // Export the menu
    gchar *sPath = g_strdup_printf ("%s/desktop", BUS_PATH);
    self->pPrivate->nExportId = g_dbus_connection_export_menu_model (pConnection, sPath, G_MENU_MODEL (self->pPrivate->pMenu), &pError);

    if (!self->pPrivate->nExportId)
    {
        g_warning ("cannot export %s menu: %s", sPath, pError->message);
        g_clear_error (&pError);
    }

    g_free (sPath);
}

static void unexport (IndicatorA11yService *self)
{
    // Unexport the menu
    if (self->pPrivate->nExportId)
    {
        g_dbus_connection_unexport_menu_model (self->pPrivate->pConnection, self->pPrivate->nExportId);
        self->pPrivate->nExportId = 0;
    }

    // Unexport the actions
    if (self->pPrivate->nActionsId)
    {
        g_dbus_connection_unexport_action_group (self->pPrivate->pConnection, self->pPrivate->nActionsId);
        self->pPrivate->nActionsId = 0;
    }
}

static void onNameLost (GDBusConnection *pConnection, const gchar *sName, gpointer pData)
{
    IndicatorA11yService *self = INDICATOR_A11Y_SERVICE (pData);

    g_debug ("%s %s name lost %s", G_STRLOC, G_STRFUNC, sName);

    unexport (self);
}

static void onDispose (GObject *pObject)
{
    IndicatorA11yService *self = INDICATOR_A11Y_SERVICE (pObject);

    if (self->pPrivate->nOnboardSubscription)
    {
        g_dbus_connection_signal_unsubscribe (self->pPrivate->pConnection, self->pPrivate->nOnboardSubscription);
    }

    if (self->pPrivate->pHighContrastSettings)
    {
        g_clear_object (&self->pPrivate->pHighContrastSettings);
    }

    if (self->pPrivate->sThemeGtk)
    {
        g_free (self->pPrivate->sThemeGtk);
    }

    if (self->pPrivate->sThemeIcon)
    {
        g_free (self->pPrivate->sThemeIcon);
    }

    if (self->pPrivate->nGreeterSubscription)
    {
        g_dbus_connection_signal_unsubscribe (self->pPrivate->pConnection, self->pPrivate->nGreeterSubscription);
    }

    if (self->pPrivate->lUsers)
    {
        g_slist_free (self->pPrivate->lUsers);
    }

    if (self->pPrivate->pOrcaSettings)
    {
        g_clear_object (&self->pPrivate->pOrcaSettings);
    }

    if (self->pPrivate->nOwnId)
    {
        g_bus_unown_name (self->pPrivate->nOwnId);
        self->pPrivate->nOwnId = 0;
    }

    unexport (self);

    g_clear_object (&self->pPrivate->pHeaderAction);
    g_clear_object (&self->pPrivate->pActionGroup);
    g_clear_object (&self->pPrivate->pConnection);

    G_OBJECT_CLASS (indicator_a11y_service_parent_class)->dispose (pObject);
}

static void onOnboardState (GSimpleAction *pAction, GVariant* pValue, gpointer pUserData)
{
    g_simple_action_set_state (pAction, pValue);

    gboolean bActive = g_variant_get_boolean (pValue);
    IndicatorA11yService *self = INDICATOR_A11Y_SERVICE (pUserData);

    if (bActive != self->pPrivate->bOnboardActive)
    {
        gchar *sFunction = NULL;

        if (bActive)
        {
            sFunction = "Show";
        }
        else
        {
            sFunction = "Hide";
        }

        GError *pError = NULL;

        if (!self->pPrivate->bGreeter)
        {
            g_dbus_connection_call_sync (self->pPrivate->pConnection, "org.onboard.Onboard", "/org/onboard/Onboard/Keyboard", "org.onboard.Onboard.Keyboard", sFunction, NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &pError);
        }
        else
        {
            GVariant *pParam = g_variant_new ("(b)", bActive);
            g_dbus_connection_call_sync (self->pPrivate->pConnection, GREETER_BUS_NAME, GREETER_BUS_PATH, GREETER_BUS_NAME, "ToggleOnBoard", pParam, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &pError);
        }

        if (pError)
        {
            g_error ("Panic: Failed to toggle Onboard: %s", pError->message);
            g_error_free (pError);

            return;
        }

        self->pPrivate->bOnboardActive = bActive;

        if (self->pPrivate->bGreeter)
        {
            loadManager (self);
        }
    }
}

static void onOrcaState (GSimpleAction *pAction, GVariant* pValue, gpointer pUserData)
{
    g_simple_action_set_state (pAction, pValue);

    IndicatorA11yService *self = INDICATOR_A11Y_SERVICE (pUserData);

    if (self->pPrivate->bGreeter)
    {
        gboolean bActive = g_variant_get_boolean (pValue);

        if (bActive != self->pPrivate->bOrcaActive)
        {
            GError *pError = NULL;
            GVariant *pParam = g_variant_new ("(b)", bActive);
            g_dbus_connection_call_sync (self->pPrivate->pConnection, GREETER_BUS_NAME, GREETER_BUS_PATH, GREETER_BUS_NAME, "ToggleOrca", pParam, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &pError);

            if (pError)
            {
                g_error ("Panic: Failed to toggle Orca: %s", pError->message);
                g_error_free (pError);

                return;
            }

            self->pPrivate->bOrcaActive = bActive;
            loadManager (self);
        }
    }
}

static void onContrastState (GSimpleAction *pAction, GVariant* pValue, gpointer pUserData)
{
    g_simple_action_set_state (pAction, pValue);
    IndicatorA11yService *self = INDICATOR_A11Y_SERVICE (pUserData);
    gboolean bActive = g_variant_get_boolean (pValue);

    if (bActive != self->pPrivate->bHighContrast)
    {
        if (!self->pPrivate->bGreeter)
        {
            self->pPrivate->bIgnoreSettings = TRUE;

            if (bActive)
            {
                g_free (self->pPrivate->sThemeGtk);
                g_free (self->pPrivate->sThemeIcon);
                self->pPrivate->sThemeGtk = g_settings_get_string (self->pPrivate->pHighContrastSettings, "gtk-theme");
                self->pPrivate->sThemeIcon = g_settings_get_string (self->pPrivate->pHighContrastSettings, "icon-theme");
                g_settings_set_string (self->pPrivate->pHighContrastSettings, "gtk-theme", "ContrastHigh");
                g_settings_set_string (self->pPrivate->pHighContrastSettings, "icon-theme", "ContrastHigh");
            }
            else
            {
                g_settings_set_string (self->pPrivate->pHighContrastSettings, "gtk-theme", self->pPrivate->sThemeGtk);
                g_settings_set_string (self->pPrivate->pHighContrastSettings, "icon-theme", self->pPrivate->sThemeIcon);
            }

            self->pPrivate->bIgnoreSettings = FALSE;
        }
        else
        {
            GError *pError = NULL;
            GVariant *pParam = g_variant_new ("(b)", bActive);
            g_dbus_connection_call_sync (self->pPrivate->pConnection, GREETER_BUS_NAME, GREETER_BUS_PATH, GREETER_BUS_NAME, "ToggleHighContrast", pParam, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &pError);

            if (pError)
            {
                g_error ("Panic: Failed to toggle high contrast: %s", pError->message);
                g_error_free (pError);

                return;
            }
        }

        self->pPrivate->bHighContrast = bActive;

        if (self->pPrivate->bGreeter)
        {
            loadManager (self);
        }
    }
}

static void onContrastSettings (GSettings *pSettings, const gchar *sKey, gpointer pUserData)
{
    IndicatorA11yService *self = INDICATOR_A11Y_SERVICE (pUserData);

    if (self->pPrivate->bIgnoreSettings)
    {
        return;
    }

    gboolean bThemeGtk = g_str_equal (sKey, "gtk-theme");
    gboolean bThemeIcon = g_str_equal (sKey, "icon-theme");

    if (bThemeGtk)
    {
        g_free (self->pPrivate->sThemeGtk);
        self->pPrivate->sThemeGtk = g_settings_get_string (self->pPrivate->pHighContrastSettings, "gtk-theme");
    }
    else if (bThemeIcon)
    {
        g_free (self->pPrivate->sThemeIcon);
        self->pPrivate->sThemeIcon = g_settings_get_string (self->pPrivate->pHighContrastSettings, "icon-theme");
    }

    bThemeGtk = g_str_equal (self->pPrivate->sThemeGtk, "ContrastHigh");
    bThemeIcon = g_str_equal (self->pPrivate->sThemeIcon, "ContrastHigh");
    gboolean bHighContrast = (bThemeGtk && bThemeIcon);

    if (self->pPrivate->bHighContrast != bHighContrast)
    {
        GAction *pAction = g_action_map_lookup_action (G_ACTION_MAP (self->pPrivate->pActionGroup), "contrast");
        GVariant *pValue = g_variant_new_boolean (bHighContrast);
        g_action_change_state (pAction, pValue);
    }
}

static gboolean valueFromVariant (GValue *pValue, GVariant *pVariant, gpointer pUserData)
{
    g_value_set_variant (pValue, pVariant);

    return TRUE;
}

static GVariant* valueToVariant (const GValue *pValue, const GVariantType *pType, gpointer pUserData)
{
    GVariant *pVariant = g_value_dup_variant (pValue);

    return pVariant;
}

static void indicator_a11y_service_init (IndicatorA11yService *self)
{
    self->pPrivate = indicator_a11y_service_get_instance_private (self);
    const char *sUser = g_get_user_name();
    self->pPrivate->bGreeter = g_str_equal (sUser, "lightdm");
    self->pPrivate->sUser = NULL;
    self->pPrivate->bOnboardActive = FALSE;
    self->pPrivate->bOrcaActive = FALSE;
    self->pPrivate->sThemeGtk = NULL;
    self->pPrivate->sThemeIcon = NULL;
    self->pPrivate->bIgnoreSettings = FALSE;
    self->pPrivate->lUsers = NULL;
    GError *pError = NULL;
    self->pPrivate->sUser = NULL;

    self->pPrivate->pConnection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &pError);

    if (pError)
    {
        g_error ("Panic: Failed connecting to the session bus: %s", pError->message);
        g_error_free (pError);

        return;
    }

    GSettingsSchemaSource *pSource = g_settings_schema_source_get_default ();
    GSettingsSchema *pSchema = NULL;

    if (!self->pPrivate->bGreeter)
    {
        // Get the settings
        if (pSource)
        {
            pSchema = g_settings_schema_source_lookup (pSource, "org.gnome.desktop.a11y.applications", FALSE);

            if (pSchema)
            {
                g_settings_schema_unref (pSchema);
                self->pPrivate->pOrcaSettings = g_settings_new ("org.gnome.desktop.a11y.applications");
            }
            else
            {
                g_error ("Panic: No org.gnome.desktop.a11y.applications schema found");
            }

            /* This is what we should use, but not all applications react to "high-contrast" setting (yet)
            pSchema = g_settings_schema_source_lookup (pSource, "org.gnome.desktop.a11y.interface", FALSE);

            if (pSchema)
            {
                g_settings_schema_unref (pSchema);
                self->pPrivate->pHighContrastSettings = g_settings_new ("org.gnome.desktop.a11y.interface");
                self->pPrivate->bHighContrast = g_settings_get_boolean (self->pPrivate->pHighContrastSettings, "high-contrast");
            }
            else
            {
                g_error ("Panic: No org.gnome.desktop.a11y.interface schema found");
            }*/

            pSchema = g_settings_schema_source_lookup (pSource, "org.mate.interface", FALSE);

            if (pSchema)
            {
                g_settings_schema_unref (pSchema);
                self->pPrivate->pHighContrastSettings = g_settings_new ("org.mate.interface");
                self->pPrivate->sThemeGtk = g_settings_get_string (self->pPrivate->pHighContrastSettings, "gtk-theme");
                self->pPrivate->sThemeIcon = g_settings_get_string (self->pPrivate->pHighContrastSettings, "icon-theme");
                gboolean bThemeGtk = g_str_equal (self->pPrivate->sThemeGtk, "ContrastHigh");
                gboolean bThemeIcon = g_str_equal (self->pPrivate->sThemeIcon, "ContrastHigh");
                self->pPrivate->bHighContrast = (bThemeGtk && bThemeIcon);
            }
            else
            {
                g_error ("Panic: No org.mate.interface schema found");
            }
        }

        self->pPrivate->nOnboardSubscription = g_dbus_connection_signal_subscribe (self->pPrivate->pConnection, NULL, "org.freedesktop.DBus.Properties", "PropertiesChanged", "/org/onboard/Onboard/Keyboard", "org.onboard.Onboard.Keyboard", G_DBUS_SIGNAL_FLAGS_MATCH_ARG0_NAMESPACE, onOnboardBus, self, NULL);
    }
    else
    {
        // Get greeter settings
        if (pSource)
        {
            pSchema = g_settings_schema_source_lookup (pSource, GREETER_SETTINGS, FALSE);

            if (pSchema)
            {
                g_settings_schema_unref (pSchema);
                GSettings *pOnboardSettings = g_settings_new (GREETER_SETTINGS);
                self->pPrivate->bOnboardActive = g_settings_get_boolean (pOnboardSettings, "onscreen-keyboard");
                g_clear_object (&pOnboardSettings);
                self->pPrivate->pOrcaSettings = g_settings_new (GREETER_SETTINGS);
                self->pPrivate->bOrcaActive = g_settings_get_boolean (self->pPrivate->pOrcaSettings, "screen-reader");
                self->pPrivate->pHighContrastSettings = g_settings_new (GREETER_SETTINGS);
                self->pPrivate->bHighContrast = g_settings_get_boolean (self->pPrivate->pHighContrastSettings, "high-contrast");
            }
            else
            {
                g_error ("Panic: No greeter schema found");
            }
        }

        self->pPrivate->nGreeterSubscription = g_dbus_connection_signal_subscribe (self->pPrivate->pConnection, NULL, GREETER_BUS_NAME, "UserChanged", GREETER_BUS_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, onUserChanged, self, NULL);
        loadManager (self);
    }

    // Create actions
    self->pPrivate->pActionGroup = g_simple_action_group_new ();

    GSimpleAction *pSimpleAction = g_simple_action_new_stateful ("_header-desktop", NULL, createHeaderState (self));
    g_action_map_add_action (G_ACTION_MAP (self->pPrivate->pActionGroup), G_ACTION (pSimpleAction));
    self->pPrivate->pHeaderAction = pSimpleAction;

    GVariant *pContrast = g_variant_new_boolean (self->pPrivate->bHighContrast);
    pSimpleAction = g_simple_action_new_stateful ("contrast", G_VARIANT_TYPE_BOOLEAN, pContrast);

    if (!self->pPrivate->bGreeter)
    {
        /* This is what we should use, but not all applications react to "high-contrast" setting (yet)
        g_settings_bind_with_mapping (self->pPrivate->pHighContrastSettings, "high-contrast", pSimpleAction, "state", G_SETTINGS_BIND_DEFAULT, valueFromVariant, valueToVariant, NULL, NULL);*/

        // Workaround for applications that do not react to "high-contrast" setting
        g_signal_connect (self->pPrivate->pHighContrastSettings, "changed::gtk-theme", G_CALLBACK (onContrastSettings), self);
        g_signal_connect (self->pPrivate->pHighContrastSettings, "changed::icon-theme", G_CALLBACK (onContrastSettings), self);
    }

    g_action_map_add_action (G_ACTION_MAP (self->pPrivate->pActionGroup), G_ACTION (pSimpleAction));
    g_signal_connect (pSimpleAction, "change-state", G_CALLBACK (onContrastState), self);
    g_object_unref (G_OBJECT (pSimpleAction));

    GVariant *pOnboard = g_variant_new_boolean (self->pPrivate->bOnboardActive);
    pSimpleAction = g_simple_action_new_stateful ("onboard", G_VARIANT_TYPE_BOOLEAN, pOnboard);
    g_action_map_add_action (G_ACTION_MAP (self->pPrivate->pActionGroup), G_ACTION (pSimpleAction));
    g_signal_connect (pSimpleAction, "change-state", G_CALLBACK (onOnboardState), self);
    g_object_unref (G_OBJECT (pSimpleAction));

    GVariant *pOrca = g_variant_new_boolean (self->pPrivate->bOrcaActive);
    pSimpleAction = g_simple_action_new_stateful ("orca", G_VARIANT_TYPE_BOOLEAN, pOrca);

    if (!self->pPrivate->bGreeter)
    {
        g_settings_bind_with_mapping (self->pPrivate->pOrcaSettings, "screen-reader-enabled", pSimpleAction, "state", G_SETTINGS_BIND_DEFAULT, valueFromVariant, valueToVariant, NULL, NULL);
    }

    g_action_map_add_action (G_ACTION_MAP (self->pPrivate->pActionGroup), G_ACTION (pSimpleAction));
    g_signal_connect (pSimpleAction, "change-state", G_CALLBACK (onOrcaState), self);
    g_object_unref (G_OBJECT (pSimpleAction));

    // Add sections to the submenu
    self->pPrivate->pSubmenu = g_menu_new();
    GMenu *pSection = g_menu_new();
    GMenuItem *pItem = NULL;

    pItem = g_menu_item_new (_("High Contrast"), "indicator.contrast");
    g_menu_item_set_attribute (pItem, "x-ayatana-type", "s", "org.ayatana.indicator.switch");
    g_menu_append_item (pSection, pItem);
    g_object_unref (pItem);

    pItem = g_menu_item_new (_("On-Screen Keyboard"), "indicator.onboard");
    g_menu_item_set_attribute (pItem, "x-ayatana-type", "s", "org.ayatana.indicator.switch");
    g_menu_append_item (pSection, pItem);
    g_object_unref (pItem);

    pItem = g_menu_item_new (_("Screen Reader"), "indicator.orca");
    g_menu_item_set_attribute (pItem, "x-ayatana-type", "s", "org.ayatana.indicator.switch");
    g_menu_append_item (pSection, pItem);
    g_object_unref (pItem);

    g_menu_append_section (self->pPrivate->pSubmenu, NULL, G_MENU_MODEL (pSection));
    g_object_unref (pSection);

    // Add submenu to the header
    pItem = g_menu_item_new (NULL, "indicator._header-desktop");
    g_menu_item_set_attribute (pItem, "x-ayatana-type", "s", "org.ayatana.indicator.root");
    g_menu_item_set_submenu (pItem, G_MENU_MODEL (self->pPrivate->pSubmenu));
    g_object_unref (self->pPrivate->pSubmenu);

    // Add header to the menu
    self->pPrivate->pMenu = g_menu_new ();
    g_menu_append_item (self->pPrivate->pMenu, pItem);
    g_object_unref (pItem);

    self->pPrivate->nOwnId = g_bus_own_name (G_BUS_TYPE_SESSION, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT, onBusAcquired, NULL, onNameLost, self, NULL);

    if (!self->pPrivate->bGreeter)
    {
        gint nUid = geteuid ();
        gchar *sPath = g_strdup_printf ("/org/freedesktop/Accounts/User%i", nUid);
        GDBusConnection *pConnection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
        GDBusProxy *pProxy = g_dbus_proxy_new_sync (pConnection, G_DBUS_PROXY_FLAGS_NONE, NULL, "org.freedesktop.Accounts", sPath, "org.freedesktop.DBus.Properties", NULL, NULL);

        if (pProxy)
        {
            const gchar *lProperties[] = {"OrcaEnabled", "OnBoardEnabled", "HighContrastEnabled"};
            const gchar *lActions[] = {"orca", "onboard", "contrast"};

            for (gint nIndex = 0; nIndex < 3; nIndex++)
            {
                GVariant *pParams = g_variant_new ("(ss)", "org.ayatana.indicator.a11y.AccountsService", lProperties[nIndex]);
                GVariant *pValue = g_dbus_proxy_call_sync (pProxy, "Get", pParams, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);

                if (pValue)
                {
                    GVariant *pChild0 = g_variant_get_child_value (pValue, 0);
                    g_variant_unref (pValue);
                    GVariant *pChild1 = g_variant_get_child_value (pChild0, 0);
                    g_variant_unref (pChild0);
                    GAction *pAction = g_action_map_lookup_action (G_ACTION_MAP (self->pPrivate->pActionGroup), lActions[nIndex]);
                    g_action_change_state (pAction, pChild1);
                    g_variant_unref (pChild1);
                }
            }
        }

        g_object_unref (pConnection);
        g_free (sPath);
    }
}

static void indicator_a11y_service_class_init (IndicatorA11yServiceClass *klass)
{
    GObjectClass *pClass = G_OBJECT_CLASS(klass);
    pClass->dispose = onDispose;
    m_nSignal = g_signal_new ("name-lost", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (IndicatorA11yServiceClass, pNameLost), NULL, NULL, g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);
}

IndicatorA11yService *indicator_a11y_service_new ()
{
    GObject *pObject = g_object_new (INDICATOR_TYPE_A11Y_SERVICE, NULL);

    return INDICATOR_A11Y_SERVICE (pObject);
}
