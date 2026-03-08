from esphome import automation
from esphome.automation import Trigger
import esphome.codegen as cg
from esphome.components.const import CONF_ON_RECEIVE
from esphome.components.packet_transport import (
    CONF_BINARY_SENSORS,
    CONF_ENCRYPTION,
    CONF_PING_PONG_ENABLE,
    CONF_PROVIDERS,
    CONF_ROLLING_CODE_ENABLE,
    CONF_SENSORS,
)
import esphome.config_validation as cv
from esphome.const import CONF_DATA, CONF_ID, CONF_PORT, CONF_TRIGGER_ID
from esphome.core import CORE, ID
from esphome.cpp_generator import MockObj
from esphome.types import ConfigType

CODEOWNERS = ["@clydebarrow"]
DEPENDENCIES = ["network"]
AUTO_LOAD = ["socket"]

MULTI_CONF = True
udp_ns = cg.esphome_ns.namespace("udp")
UDPComponent = udp_ns.class_("UDPComponent", cg.Component)
UDPWriteAction = udp_ns.class_("UDPWriteAction", automation.Action)
trigger_argname = "data"
# Listener callback type (non-owning span from UDP component)
listener_args = cg.std_vector.template(cg.uint8)
listener_argtype = [(listener_args, trigger_argname)]
# Automation/trigger type (owned vector, safe for deferred actions like delay)
trigger_args = cg.std_vector.template(cg.uint8)
trigger_argtype = [(trigger_args, trigger_argname)]

CONF_ADDRESSES = "addresses"
CONF_LISTEN_ADDRESS = "listen_address"
CONF_UDP_ID = "udp_id"
CONF_LISTEN_PORT = "listen_port"
CONF_BROADCAST_PORT = "broadcast_port"
CONF_ENABLE_IPV6 = "enable_ipv6"  # NEW: Added for IPv6 support

UDP_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_UDP_ID): cv.use_id(UDPComponent),
    }
)


def is_relocated(option):
    def validator(value):
        raise cv.Invalid(
            f"The '{option}' option should now be configured in the 'packet_transport' component"
        )

    return validator


RELOCATED = {
    cv.Optional(x): is_relocated(x)
    for x in (
        CONF_PROVIDERS,
        CONF_ENCRYPTION,
        CONF_PING_PONG_ENABLE,
        CONF_ROLLING_CODE_ENABLE,
        CONF_SENSORS,
        CONF_BINARY_SENSORS,
    )
}


def _consume_udp_sockets(config: ConfigType) -> ConfigType:
    """Register socket needs for UDP component."""
    from esphome.components import socket

    # UDP uses up to 2 sockets: 1 broadcast + 1 listen
    # Whether each is used depends on code generation, so register worst case
    socket.consume_sockets(2, "udp", socket.SocketType.UDP)(config)
    return config


# NEW: IPv6 validation function
def validate_ipv6_support(config):
    """Validate IPv6 configuration for ESP32-C6 platforms."""
    if config.get(CONF_ENABLE_IPV6, False):
        if CORE.is_esp32:
            from esphome.components.esp32 import get_esp32_variant
            from esphome.components.esp32.const import (
                VARIANT_ESP32C6,
                VARIANT_ESP32H2,
            )
            
            variant = get_esp32_variant()
            if variant not in [VARIANT_ESP32C6, VARIANT_ESP32H2]:
                raise cv.Invalid(
                    f"IPv6 is only supported on ESP32-C6 and ESP32-H2, not on {variant}"
                )
        else:
            raise cv.Invalid("IPv6 is only supported on ESP32-C6 and ESP32-H2 platforms")
    return config


CONFIG_SCHEMA = cv.All(
    cv.COMPONENT_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(UDPComponent),
            cv.Optional(CONF_PORT, default=18511): cv.Any(
                cv.port,
                cv.Schema(
                    {
                        cv.Required(CONF_LISTEN_PORT): cv.port,
                        cv.Required(CONF_BROADCAST_PORT): cv.port,
                    }
                ),
            ),
            cv.Optional(
                CONF_LISTEN_ADDRESS, default="255.255.255.255"
            ): cv.Any(cv.ipv4address_multi_broadcast, cv.ipv6address),  # Allow both IPv4 and IPv6
            cv.Optional(CONF_ADDRESSES, default=["255.255.255.255"]): cv.ensure_list(
                cv.Any(cv.ipv4address, cv.ipv6address),  # MODIFIED: Added IPv6 address support
            ),
            cv.Optional(CONF_ON_RECEIVE): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        Trigger.template(trigger_args)
                    ),
                }
            ),
            cv.Optional(CONF_ENABLE_IPV6, default=False): cv.boolean,  # NEW: IPv6 enable option
        }
    ).extend(RELOCATED),
    _consume_udp_sockets,
    validate_ipv6_support,  # NEW: IPv6 validation
)


async def register_udp_client(var, config):
    udp_var = await cg.get_variable(config[CONF_UDP_ID])
    cg.add(var.set_parent(udp_var))
    return udp_var


async def to_code(config):
    cg.add_define("USE_UDP")
    cg.add_global(udp_ns.using)
    var = cg.new_Pvariable(config[CONF_ID])
    var = await cg.register_component(var, config)
    conf_port = config[CONF_PORT]
    if isinstance(conf_port, int):
        cg.add(var.set_listen_port(conf_port))
        cg.add(var.set_broadcast_port(conf_port))
    else:
        cg.add(var.set_listen_port(conf_port[CONF_LISTEN_PORT]))
        cg.add(var.set_broadcast_port(conf_port[CONF_BROADCAST_PORT]))
    if (listen_address := str(config[CONF_LISTEN_ADDRESS])) != "255.255.255.255":
        cg.add(var.set_listen_address(listen_address))
    cg.add(var.set_addresses([str(addr) for addr in config[CONF_ADDRESSES]]))
    if on_receive := config.get(CONF_ON_RECEIVE):
        on_receive = on_receive[0]
        trigger_id = cg.new_Pvariable(on_receive[CONF_TRIGGER_ID])
        trigger = await automation.build_automation(
            trigger_id, trigger_argtype, on_receive
        )
        trigger_lambda = await cg.process_lambda(
            trigger.trigger(
                cg.std_vector.template(cg.uint8)(
                    MockObj(trigger_argname).begin(),
                    MockObj(trigger_argname).end(),
                )
            ),
            listener_argtype,
        )
        cg.add(var.add_listener(trigger_lambda))
        cg.add(var.set_should_listen())
    
    # NEW: IPv6 support configuration for ESP32-C6
    enable_ipv6 = config.get(CONF_ENABLE_IPV6, False)
    if enable_ipv6:
        cg.add_define("USE_UDP_IPV6")
        
        if CORE.is_esp32 and CORE.using_esp_idf:
            from esphome.components.esp32 import add_idf_sdkconfig_option
            
            # Enable IPv6 in ESP-IDF LWIP stack
            add_idf_sdkconfig_option("CONFIG_LWIP_IPV6", True)
            add_idf_sdkconfig_option("CONFIG_LWIP_IPV6_AUTOCONFIG", True)
            add_idf_sdkconfig_option("CONFIG_LWIP_IPV6_NUM_ADDRESSES", 3)
            add_idf_sdkconfig_option("CONFIG_LWIP_IPV6_FRAG", True)
            add_idf_sdkconfig_option("CONFIG_LWIP_ND6_QUEUEING", True)


def validate_raw_data(value):
    if isinstance(value, str):
        return value.encode("utf-8")
    if isinstance(value, str):
        return value
    if isinstance(value, list):
        return cv.Schema([cv.hex_uint8_t])(value)
    raise cv.Invalid(
        "data must either be a string wrapped in quotes or a list of bytes"
    )


@automation.register_action(
    "udp.write",
    UDPWriteAction,
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(UDPComponent),
            cv.Required(CONF_DATA): cv.templatable(validate_raw_data),
        },
        key=CONF_DATA,
    ),
)
async def udp_write_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    udp_var = await cg.get_variable(config[CONF_ID])
    await cg.register_parented(var, udp_var)
    cg.add(udp_var.set_should_broadcast())
    data = config[CONF_DATA]
    if isinstance(data, bytes):
        data = list(data)

    if cg.is_template(data):
        templ = await cg.templatable(data, args, cg.std_vector.template(cg.uint8))
        cg.add(var.set_data_template(templ))
    else:
        # Generate static array in flash to avoid RAM copy
        arr_id = ID(f"{action_id}_data", is_declaration=True, type=cg.uint8)
        arr = cg.static_const_array(arr_id, cg.ArrayInitializer(*data))
        cg.add(var.set_data_static(arr, len(data)))
    return var
