import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F


class Net(nn.Module):
    def __init__(self, s_dim, a_dim, hidden_dim=128):
        super(Net, self).__init__()
        self.s_dim = s_dim
        self.a_dim = a_dim
        self.pi1 = nn.Linear(s_dim, hidden_dim)
        self.pi2 = nn.Linear(hidden_dim, a_dim)
        self.v1 = nn.Linear(s_dim, hidden_dim)
        self.pi_hidden = nn.Linear(hidden_dim, hidden_dim)
        self.v_hidden = nn.Linear(hidden_dim, hidden_dim)
        self.v_norm = nn.LayerNorm(hidden_dim)
        self.v2 = nn.Linear(hidden_dim, 1)
        self.auxiliary = nn.Linear(hidden_dim + a_dim, 8)
        set_init([self.pi1, self.pi_hidden, self.pi2, self.v1, self.v_hidden, self.v2, self.auxiliary])
        self.distribution = torch.distributions.Categorical

    def _value_features(self, x):
        return F.relu(self.v_hidden(self.v_norm(F.relu(self.v1(x)))))

    def forward(self, x):
        logits = self.pi2(F.relu(self.pi_hidden(F.relu(self.pi1(x)))))
        values = self.v2(self._value_features(x))
        return logits, values

    def behavior_distribution(self, logits, temperature=1.0, epsilon=0.0, exploration_bonus=None):
        temperature = torch.as_tensor(temperature, dtype=logits.dtype, device=logits.device)
        epsilon = torch.as_tensor(epsilon, dtype=logits.dtype, device=logits.device)
        if torch.any(temperature <= 0) or torch.any((epsilon < 0) | (epsilon > 1)):
            raise ValueError("invalid policy temperature or epsilon")
        bonus = 0.0 if exploration_bonus is None else torch.as_tensor(exploration_bonus, dtype=logits.dtype, device=logits.device)
        probabilities = (1.0 - epsilon) * F.softmax((logits + bonus) / temperature, dim=1) + epsilon / self.a_dim
        return self.distribution(probs=probabilities)

    def choose_action(self, s, temperature=1.0, epsilon=0.0, exploration_bonus=None, return_info=False):
        self.eval()
        with torch.no_grad():
            logits, _ = self.forward(s)
            distribution = self.behavior_distribution(logits, temperature, epsilon, exploration_bonus)
            sampled = distribution.sample()
            if return_info:
                if sampled.numel() != 1:
                    raise ValueError("sampling metadata requires one decision")
                info = {"temperature": float(temperature), "epsilon": float(epsilon),
                        "exploration_bonus": np.zeros(self.a_dim).tolist() if exploration_bonus is None else np.asarray(exploration_bonus).tolist(),
                        "log_prob": float(distribution.log_prob(sampled).item())}
                return sampled, info
            return sampled

    def loss_func(self, s, a, v_t, entropy_beta=0.0, value_loss_weight=0.5,
                  behavior=None, auxiliary_targets=None, auxiliary_weight=0.1):
        self.train()
        logits, values = self.forward(s)
        advantage = (v_t - values).detach().squeeze(-1)
        if behavior is not None:
            distribution = self.behavior_distribution(
                logits,
                [[item["temperature"]] for item in behavior],
                [[item["epsilon"]] for item in behavior],
                [item["exploration_bonus"] for item in behavior],
            )
            old_log = torch.tensor([item["log_prob"] for item in behavior], dtype=logits.dtype, device=logits.device)
            ratio = torch.exp(distribution.log_prob(a) - old_log)
            if advantage.numel() > 1 and advantage.std(unbiased=False) > 1e-6:
                advantage = (advantage - advantage.mean()) / (advantage.std(unbiased=False) + 1e-6)
            actor_loss = -torch.minimum(ratio * advantage, ratio.clamp(0.8, 1.2) * advantage).mean()
        else:
            distribution = self.distribution(logits=logits)
            actor_loss = -(distribution.log_prob(a) * advantage).mean()
        critic_loss = F.smooth_l1_loss(values, v_t)
        auxiliary_loss = torch.zeros((), dtype=values.dtype, device=values.device)
        if auxiliary_targets is not None:
            encoded = torch.cat((self._value_features(s), F.one_hot(a, self.a_dim).to(s.dtype)), dim=1)
            predicted = self.auxiliary(encoded)
            for column, key in enumerate(("valid", "pta", "target")):
                labels = torch.tensor([item.get(key, -1) for item in auxiliary_targets], dtype=s.dtype, device=s.device)
                mask = labels >= 0
                # No invented positive examples: the sparse hit head is trained
                # once a batch actually contains a confirmed target.
                if mask.any() and (key != "target" or (labels[mask] > 0).any()):
                    auxiliary_loss = auxiliary_loss + F.binary_cross_entropy_with_logits(predicted[mask, column], labels[mask])
            labels = torch.tensor([item.get("stage", -1) for item in auxiliary_targets], dtype=torch.long, device=s.device)
            mask = labels >= 0
            if mask.any():
                auxiliary_loss = auxiliary_loss + F.cross_entropy(predicted[mask, 3:], labels[mask])
        return actor_loss + value_loss_weight * critic_loss + auxiliary_weight * auxiliary_loss - entropy_beta * distribution.entropy().mean()


def v_wrap(np_array, dtype=np.float32):
    if np_array.dtype != dtype:
        np_array = np_array.astype(dtype)
    return torch.from_numpy(np_array)


def set_init(layers):
    for layer in layers:
        nn.init.normal_(layer.weight, mean=0., std=0.1)
        nn.init.constant_(layer.bias, 0.)
