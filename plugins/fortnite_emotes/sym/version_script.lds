{
    global:
        Plugify_*;

        /* s2sdk binding slots (defined by s2sdk_bindings, filled by the plugify module) */
        __s2sdk_*;

        /* admin_system binding slots (defined by admin_system_bindings, filled by the plugify
           module; the dependency is optional, the checks degrade to "no access") */
        __admin_system_*;

        /* vip binding slots (defined by vip_bindings; optional dependency) */
        __vip_*;

        /* hudmenu binding slots - pisexcompat resolves every displayed menu through
           __hudmenu_GetClientMenuType when the hudmenu plugin is installed */
        __hudmenu_*;

        /* No exported methods: fortnite_emotes exposes nothing to other plugins. */

    local: *;
};
