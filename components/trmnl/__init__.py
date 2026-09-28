import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import binary_sensor, sensor
from esphome.components.http_request import CONF_HTTP_REQUEST_ID, HttpRequestComponent
from esphome.const import CONF_HEIGHT, CONF_ID, CONF_MODEL, CONF_ON_ERROR, CONF_WIDTH
from esphome.types import ConfigType

CODEOWNERS = ["@paveldn"]
DEPENDENCIES = ["http_request"]
AUTO_LOAD = ["json"]
MULTI_CONF = True

trmnl_ns = cg.esphome_ns.namespace("trmnl")
TrmnlComponent = trmnl_ns.class_("TrmnlComponent", cg.Component)
TrmnlUpdateAction = trmnl_ns.class_(
    "TrmnlUpdateAction", automation.Action, cg.Parented.template(TrmnlComponent)
)
TrmnlTriggerSpecialFunctionAction = trmnl_ns.class_(
    "TrmnlTriggerSpecialFunctionAction",
    automation.Action,
    cg.Parented.template(TrmnlComponent),
)

CONF_BASE_URL = "base_url"
CONF_API_KEY = "api_key"
CONF_VOLTAGE_SENSOR = "voltage_sensor"
CONF_BATTERY_LEVEL_SENSOR = "battery_level_sensor"
CONF_CHARGING_BINARY_SENSOR = "charging_binary_sensor"

CONF_ON_IMAGE_AVAILABLE = "on_image_available"


def validate_base_url(value: str) -> str:
    value = cv.url(value)
    if not value.startswith(("http://", "https://")):
        raise cv.Invalid("base_url must start with 'http://' or 'https://'")
    return value.rstrip("/")


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(TrmnlComponent),
        cv.GenerateID(CONF_HTTP_REQUEST_ID): cv.use_id(HttpRequestComponent),
        cv.Required(CONF_BASE_URL): validate_base_url,
        cv.Optional(CONF_API_KEY, default=""): cv.sensitive(cv.string_strict),
        cv.Optional(CONF_MODEL, default="esphome"): cv.string_strict,
        cv.Optional(CONF_WIDTH): cv.int_range(min=1),
        cv.Optional(CONF_HEIGHT): cv.int_range(min=1),
        cv.Optional(CONF_VOLTAGE_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_BATTERY_LEVEL_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_CHARGING_BINARY_SENSOR): cv.use_id(binary_sensor.BinarySensor),
        cv.Optional(CONF_ON_IMAGE_AVAILABLE): automation.validate_automation({}),
        cv.Optional(CONF_ON_ERROR): automation.validate_automation({}),
    }
).extend(cv.COMPONENT_SCHEMA)

_CALLBACK_AUTOMATIONS = (
    automation.CallbackAutomation(
        CONF_ON_IMAGE_AVAILABLE,
        "add_on_image_available_callback",
        [
            (cg.std_string, "image_url"),
            (cg.std_string, "filename"),
            (cg.uint32, "refresh_rate"),
            (cg.bool_, "image_changed"),
        ],
    ),
    automation.CallbackAutomation(
        CONF_ON_ERROR,
        "add_on_error_callback",
        [(cg.std_string, "error_type")],
    ),
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_HTTP_REQUEST_ID])

    cg.add(var.set_base_url(config[CONF_BASE_URL]))
    if config[CONF_API_KEY]:
        cg.add(var.set_api_key(config[CONF_API_KEY]))
    cg.add(var.set_model(config[CONF_MODEL]))
    if CONF_WIDTH in config:
        cg.add(var.set_width(config[CONF_WIDTH]))
    if CONF_HEIGHT in config:
        cg.add(var.set_height(config[CONF_HEIGHT]))

    if CONF_VOLTAGE_SENSOR in config:
        sens = await cg.get_variable(config[CONF_VOLTAGE_SENSOR])
        cg.add(var.set_voltage_sensor(sens))
    if CONF_BATTERY_LEVEL_SENSOR in config:
        sens = await cg.get_variable(config[CONF_BATTERY_LEVEL_SENSOR])
        cg.add(var.set_battery_level_sensor(sens))
    if CONF_CHARGING_BINARY_SENSOR in config:
        sens = await cg.get_variable(config[CONF_CHARGING_BINARY_SENSOR])
        cg.add(var.set_charging_binary_sensor(sens))

    await automation.build_callback_automations(var, config, _CALLBACK_AUTOMATIONS)


UPDATE_ACTION_SCHEMA = automation.maybe_simple_id(
    {
        cv.GenerateID(): cv.use_id(TrmnlComponent),
    }
)


@automation.register_action(
    "trmnl.update", TrmnlUpdateAction, UPDATE_ACTION_SCHEMA, synchronous=True
)
async def trmnl_update_action_to_code(
    config: ConfigType, action_id, template_arg, args
):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


TRIGGER_SPECIAL_FUNCTION_ACTION_SCHEMA = automation.maybe_simple_id(
    {
        cv.GenerateID(): cv.use_id(TrmnlComponent),
    }
)


@automation.register_action(
    "trmnl.trigger_special_function",
    TrmnlTriggerSpecialFunctionAction,
    TRIGGER_SPECIAL_FUNCTION_ACTION_SCHEMA,
    synchronous=True,
)
async def trmnl_trigger_special_function_action_to_code(
    config: ConfigType, action_id, template_arg, args
):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)
