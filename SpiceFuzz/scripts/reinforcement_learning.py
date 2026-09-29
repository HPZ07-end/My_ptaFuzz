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
        self.v2 = nn.Linear(hidden_dim, 1)
        set_init([self.pi1, self.pi2, self.v1, self.v2])
        self.distribution = torch.distributions.Categorical

    def forward(self, x):
        pi1 = F.relu(self.pi1(x))
        logits = self.pi2(pi1)
        v1 = F.relu(self.v1(x))
        values = self.v2(v1)
        return logits, values

    def choose_action(
        self,
        s,
        temperature=1.0,
        epsilon=0.0,
        exploration_bonus=None,
    ):
        if temperature <= 0:
            raise ValueError("policy temperature must be positive")
        if not 0.0 <= epsilon <= 1.0:
            raise ValueError("epsilon must be between 0 and 1")
        self.eval()
        with torch.no_grad():
            logits, _ = self.forward(s)
            if exploration_bonus is not None:
                bonus = torch.as_tensor(
                    exploration_bonus,
                    dtype=logits.dtype,
                    device=logits.device,
                )
                logits = logits + bonus
            probabilities = F.softmax(logits / temperature, dim=1)
            sampled = self.distribution(probabilities).sample()
            if epsilon > 0.0:
                random_actions = torch.randint(
                    self.a_dim,
                    sampled.shape,
                    device=sampled.device,
                )
                explore = torch.rand(sampled.shape, device=sampled.device) < epsilon
                sampled = torch.where(explore, random_actions, sampled)
            return sampled

    def loss_func(
        self,
        s,
        a,
        v_t,
        entropy_beta=0.0,
        value_loss_weight=0.5,
    ):
        self.train()
        logits, values = self.forward(s)
        advantage = v_t - values
        distribution = self.distribution(logits=logits)
        actor_loss = -(
            distribution.log_prob(a) * advantage.detach().squeeze(-1)
        ).mean()
        critic_loss = advantage.pow(2).mean()
        entropy = distribution.entropy().mean()
        return (
            actor_loss
            + value_loss_weight * critic_loss
            - entropy_beta * entropy
        )


def v_wrap(np_array, dtype=np.float32):
    if np_array.dtype != dtype:
        np_array = np_array.astype(dtype)
    return torch.from_numpy(np_array)


def set_init(layers):
    for layer in layers:
        nn.init.normal_(layer.weight, mean=0., std=0.1)
        nn.init.constant_(layer.bias, 0.)
