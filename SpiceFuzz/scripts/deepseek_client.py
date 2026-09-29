import os

import httpx


DEFAULT_DEEPSEEK_BASE_URL = "https://api.deepseek.com"
DEFAULT_DEEPSEEK_MODEL = "deepseek-flash"


class DeepSeekAPIError(RuntimeError):
    pass


def deepseek_settings_from_environment():
    return {
        "api_key": os.getenv("DEEPSEEK_API_KEY"),
        "base_url": os.getenv(
            "DEEPSEEK_BASE_URL",
            DEFAULT_DEEPSEEK_BASE_URL,
        ),
        "model": os.getenv("DEEPSEEK_MODEL", DEFAULT_DEEPSEEK_MODEL),
    }


def validate_deepseek_api_key(api_key):
    if not api_key:
        raise ValueError(
            "Set DEEPSEEK_API_KEY or pass --deepseek_api_key"
        )
    api_key = api_key.strip()
    placeholders = {
        "你的api密钥",
        "your_api_key_here",
        "<your deepseek api key>",
        "********",
        "*********",
    }
    if api_key.lower() in placeholders:
        raise ValueError(
            "DEEPSEEK_API_KEY is still a placeholder; replace it with a real "
            "DeepSeek API key"
        )
    try:
        api_key.encode("ascii")
    except UnicodeEncodeError as error:
        raise ValueError(
            "DEEPSEEK_API_KEY must contain only ASCII characters; do not use "
            "the Chinese placeholder text"
        ) from error
    return api_key


class DeepSeekClient:
    def __init__(
        self,
        api_key,
        base_url=DEFAULT_DEEPSEEK_BASE_URL,
        timeout=120.0,
        transport=None,
    ):
        self.api_key = validate_deepseek_api_key(api_key)
        self.base_url = str(base_url or DEFAULT_DEEPSEEK_BASE_URL).rstrip("/")
        if not self.base_url.startswith(("https://", "http://")):
            raise ValueError("DEEPSEEK_BASE_URL must start with http:// or https://")
        client_options = {
            "timeout": timeout,
            "headers": {
                "Authorization": f"Bearer {self.api_key}",
                "Content-Type": "application/json",
            },
        }
        if transport is not None:
            client_options["transport"] = transport
        self._client = httpx.Client(
            **client_options,
        )
        self.last_finish_reason = None

    def create_chat_completion(
        self,
        messages,
        model=DEFAULT_DEEPSEEK_MODEL,
        temperature=0.8,
        max_tokens=2500,
        thinking=False,
    ):
        payload = {
            "model": model,
            "messages": messages,
            "temperature": temperature,
            "max_tokens": max_tokens,
            "stream": False,
            # Netlist generation is a strict serialization task. DeepSeek's
            # thinking mode is enabled by default and can spend the output
            # budget on reasoning before emitting the complete deck.
            "thinking": {"type": "enabled" if thinking else "disabled"},
        }
        try:
            response = self._client.post(
                f"{self.base_url}/chat/completions",
                json=payload,
            )
        except httpx.HTTPError as error:
            raise DeepSeekAPIError(f"DeepSeek request failed: {error}") from error

        if response.is_error:
            detail = response.text[-2000:]
            raise DeepSeekAPIError(
                f"DeepSeek API returned HTTP {response.status_code}: {detail}"
            )
        try:
            body = response.json()
            choice = body["choices"][0]
            self.last_finish_reason = choice.get("finish_reason")
            return choice["message"]["content"] or ""
        except (KeyError, IndexError, TypeError, ValueError) as error:
            raise DeepSeekAPIError(
                "DeepSeek response did not contain choices[0].message.content"
            ) from error

    def close(self):
        self._client.close()
